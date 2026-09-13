#include "cake.h"

#include <errno.h>
#include <limits.h>
#include <linux/gen_stats.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct cake_dump_context {
    unsigned int interface_index;
    struct cake_observation *observation;
    bool found;
};

static void set_error(
    char *error,
    size_t error_size,
    const char *format,
    ...
)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static const struct rtattr *next_attribute(
    const struct rtattr *attribute,
    size_t *remaining
)
{
    size_t attribute_length;
    size_t aligned_length;

    if (*remaining < sizeof(*attribute)) {
        return NULL;
    }

    attribute_length = attribute->rta_len;
    if (attribute_length < sizeof(*attribute) ||
        attribute_length > *remaining) {
        return NULL;
    }

    aligned_length = RTA_ALIGN(attribute_length);
    if (aligned_length > *remaining) {
        *remaining = 0U;
        return NULL;
    }

    *remaining -= aligned_length;
    return (const struct rtattr *)(
        (const unsigned char *)attribute + aligned_length
    );
}

static const struct rtattr *find_attribute(
    const void *data,
    size_t length,
    unsigned short type
)
{
    const struct rtattr *attribute = data;
    size_t remaining = length;

    while (remaining >= sizeof(*attribute)) {
        if (attribute->rta_len < sizeof(*attribute) ||
            (size_t)attribute->rta_len > remaining) {
            return NULL;
        }

        if ((attribute->rta_type & NLA_TYPE_MASK) == type) {
            return attribute;
        }

        attribute = next_attribute(attribute, &remaining);
        if (attribute == NULL) {
            break;
        }
    }

    return NULL;
}

static const void *attribute_data(const struct rtattr *attribute)
{
    return (const unsigned char *)attribute + sizeof(*attribute);
}

static size_t attribute_payload(const struct rtattr *attribute)
{
    return (size_t)attribute->rta_len - sizeof(*attribute);
}

static bool read_u32(
    const struct rtattr *attribute,
    uint32_t *value
)
{
    if (attribute == NULL || attribute_payload(attribute) < sizeof(*value)) {
        return false;
    }

    memcpy(value, attribute_data(attribute), sizeof(*value));
    return true;
}

static bool read_u64(
    const struct rtattr *attribute,
    uint64_t *value
)
{
    if (attribute == NULL || attribute_payload(attribute) < sizeof(*value)) {
        return false;
    }

    memcpy(value, attribute_data(attribute), sizeof(*value));
    return true;
}

static uint64_t rate_to_bits_per_second(uint64_t bytes_per_second)
{
    if (bytes_per_second > UINT64_MAX / 8U) {
        return UINT64_MAX;
    }

    /* CAKE's netlink ABI reports rates in bytes/s; sqm-mon uses bits/s. */
    return bytes_per_second * 8U;
}

static bool kind_is_cake(const struct rtattr *kind)
{
    const char expected[] = "cake";
    size_t payload;

    if (kind == NULL) {
        return false;
    }

    payload = attribute_payload(kind);
    return payload >= sizeof(expected) &&
        memcmp(attribute_data(kind), expected, sizeof(expected)) == 0;
}

static void parse_options(
    const struct rtattr *options,
    struct cake_observation *observation
)
{
    const struct rtattr *bandwidth;
    uint64_t rate;

    if (options == NULL) {
        return;
    }

    bandwidth = find_attribute(
        attribute_data(options),
        attribute_payload(options),
        TCA_CAKE_BASE_RATE64
    );
    if (read_u64(bandwidth, &rate)) {
        observation->bandwidth_bits_per_second =
            rate_to_bits_per_second(rate);
        observation->has_bandwidth = true;
    }
}

static void parse_basic_stats(
    const struct rtattr *basic,
    struct cake_observation *observation
)
{
    const unsigned char *data;
    size_t payload;

    if (basic == NULL) {
        return;
    }

    data = attribute_data(basic);
    payload = attribute_payload(basic);
    if (payload < sizeof(uint64_t) + sizeof(uint32_t)) {
        return;
    }

    /* TCA_STATS_BASIC is the kernel ABI pair: u64 bytes, then u32 packets. */
    memcpy(&observation->bytes, data, sizeof(observation->bytes));
    memcpy(
        &observation->packets,
        data + sizeof(observation->bytes),
        sizeof(observation->packets)
    );
    observation->has_basic_stats = true;
}

static void parse_queue_stats(
    const struct rtattr *queue,
    struct cake_observation *observation
)
{
    struct gnet_stats_queue stats;

    if (queue == NULL || attribute_payload(queue) < sizeof(stats)) {
        return;
    }

    memcpy(&stats, attribute_data(queue), sizeof(stats));
    observation->queue_length = stats.qlen;
    observation->backlog_bytes = stats.backlog;
    observation->drops = stats.drops;
    observation->has_queue_stats = true;
}

static void parse_cake_stats(
    const struct rtattr *application,
    struct cake_observation *observation
)
{
    const struct rtattr *capacity;
    const struct rtattr *memory_limit;
    const struct rtattr *memory_used;
    uint64_t rate;
    bool has_memory_limit;
    bool has_memory_used;

    if (application == NULL) {
        return;
    }

    capacity = find_attribute(
        attribute_data(application),
        attribute_payload(application),
        TCA_CAKE_STATS_CAPACITY_ESTIMATE64
    );
    if (read_u64(capacity, &rate)) {
        observation->capacity_estimate_bits_per_second =
            rate_to_bits_per_second(rate);
        observation->has_capacity_estimate = true;
    }

    memory_limit = find_attribute(
        attribute_data(application),
        attribute_payload(application),
        TCA_CAKE_STATS_MEMORY_LIMIT
    );
    memory_used = find_attribute(
        attribute_data(application),
        attribute_payload(application),
        TCA_CAKE_STATS_MEMORY_USED
    );
    has_memory_limit = read_u32(
        memory_limit,
        &observation->memory_limit_bytes
    );
    has_memory_used = read_u32(
        memory_used,
        &observation->memory_used_bytes
    );
    observation->has_memory_stats = has_memory_limit && has_memory_used;
}

static void parse_stats(
    const struct rtattr *stats,
    struct cake_observation *observation
)
{
    if (stats == NULL) {
        return;
    }

    parse_basic_stats(
        find_attribute(
            attribute_data(stats),
            attribute_payload(stats),
            TCA_STATS_BASIC
        ),
        observation
    );
    parse_queue_stats(
        find_attribute(
            attribute_data(stats),
            attribute_payload(stats),
            TCA_STATS_QUEUE
        ),
        observation
    );
    parse_cake_stats(
        find_attribute(
            attribute_data(stats),
            attribute_payload(stats),
            TCA_STATS_APP
        ),
        observation
    );
}

static int handle_qdisc(
    const struct nlmsghdr *message,
    void *context_pointer
)
{
    struct cake_dump_context *context = context_pointer;
    const struct tcmsg *traffic_control;
    const struct rtattr *attributes;
    const struct rtattr *kind;
    size_t attributes_length;

    if (message->nlmsg_len < NLMSG_LENGTH(sizeof(struct tcmsg))) {
        return -1;
    }

    traffic_control = NLMSG_DATA(message);
    /* Only control the interface's root CAKE, never a nested child qdisc. */
    if (traffic_control->tcm_ifindex != (int)context->interface_index ||
        traffic_control->tcm_parent != TC_H_ROOT ||
        context->found) {
        return 0;
    }

    attributes = (const struct rtattr *)(
        (const unsigned char *)traffic_control +
        NLMSG_ALIGN(sizeof(*traffic_control))
    );
    attributes_length =
        (size_t)message->nlmsg_len - NLMSG_LENGTH(sizeof(*traffic_control));
    kind = find_attribute(attributes, attributes_length, TCA_KIND);
    if (!kind_is_cake(kind)) {
        return 0;
    }

    memset(context->observation, 0, sizeof(*context->observation));
    context->observation->handle = traffic_control->tcm_handle;
    context->observation->parent = traffic_control->tcm_parent;
    parse_options(
        find_attribute(attributes, attributes_length, TCA_OPTIONS),
        context->observation
    );
    parse_stats(
        find_attribute(attributes, attributes_length, TCA_STATS2),
        context->observation
    );
    context->found = true;
    return 0;
}

enum cake_read_result cake_read(
    struct sqm_mon_netlink *netlink,
    const char *interface,
    struct cake_observation *observation,
    char *error,
    size_t error_size
)
{
    struct cake_dump_context context;
    unsigned int interface_index;

    errno = 0;
    interface_index = if_nametoindex(interface);
    if (interface_index == 0U) {
        set_error(
            error,
            error_size,
            "could not find interface '%s': %s",
            interface,
            errno == 0 ? "unknown interface" : strerror(errno)
        );
        return CAKE_READ_ERROR;
    }

    if (netlink_open(netlink, error, error_size) != 0) {
        return CAKE_READ_ERROR;
    }

    context.interface_index = interface_index;
    context.observation = observation;
    context.found = false;
    if (netlink_dump_qdiscs(
            netlink,
            interface_index,
            handle_qdisc,
            &context,
            error,
            error_size
        ) != 0) {
        netlink_close(netlink);
        return CAKE_READ_ERROR;
    }

    return context.found ? CAKE_READ_FOUND : CAKE_READ_NOT_FOUND;
}

int cake_set_bandwidth(
    struct sqm_mon_netlink *netlink,
    const char *interface,
    const struct cake_observation *observation,
    uint64_t bandwidth_bits_per_second,
    char *error,
    size_t error_size
)
{
    uint64_t bandwidth_bytes_per_second;
    unsigned int interface_index;

    if (observation == NULL) {
        set_error(error, error_size, "CAKE observation is null");
        return -1;
    }
    if (bandwidth_bits_per_second < 8U ||
        bandwidth_bits_per_second % 8U != 0U) {
        set_error(
            error,
            error_size,
            "CAKE bandwidth must be a positive multiple of 8 bit/s"
        );
        return -1;
    }

    errno = 0;
    interface_index = if_nametoindex(interface);
    if (interface_index == 0U) {
        set_error(
            error,
            error_size,
            "could not find interface '%s': %s",
            interface,
            errno == 0 ? "unknown interface" : strerror(errno)
        );
        return -1;
    }
    if (netlink_open(netlink, error, error_size) != 0) {
        return -1;
    }

    bandwidth_bytes_per_second = bandwidth_bits_per_second / 8U;
    if (netlink_change_qdisc_option(
            netlink,
            interface_index,
            observation->handle,
            observation->parent,
            "cake",
            TCA_CAKE_BASE_RATE64,
            &bandwidth_bytes_per_second,
            sizeof(bandwidth_bytes_per_second),
            error,
            error_size
        ) != 0) {
        netlink_close(netlink);
        return -1;
    }

    return 0;
}
