#define _GNU_SOURCE

#include "cake.h"
#include "error.h"

#include <errno.h>
#include <limits.h>
#include <linux/gen_stats.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netlink/attr.h>
#include <string.h>

struct cake_dump_context {
    unsigned int interface_index;
    struct cake_observation *observation;
    bool found;
};

static struct nlattr *find_attribute(
    const void *data,
    size_t length,
    unsigned short type
)
{
    /* Kernel message lengths are external input; nla_find takes an int. */
    if (length > INT_MAX) {
        return NULL;
    }
    return nla_find((struct nlattr *)(void *)data, (int)length, type);
}

static bool read_u32(
    const struct nlattr *attribute,
    uint32_t *value
)
{
    if (attribute == NULL || nla_len(attribute) < (int)sizeof(*value)) {
        return false;
    }

    memcpy(value, nla_data(attribute), sizeof(*value));
    return true;
}

static bool read_u64(
    const struct nlattr *attribute,
    uint64_t *value
)
{
    if (attribute == NULL || nla_len(attribute) < (int)sizeof(*value)) {
        return false;
    }

    memcpy(value, nla_data(attribute), sizeof(*value));
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

static bool kind_is_cake(const struct nlattr *kind)
{
    const char expected[] = "cake";
    size_t payload;

    if (kind == NULL) {
        return false;
    }

    payload = (size_t)nla_len(kind);
    return payload >= sizeof(expected) &&
        memcmp(nla_data(kind), expected, sizeof(expected)) == 0;
}

static void parse_options(
    const struct nlattr *options,
    struct cake_observation *observation
)
{
    const struct nlattr *bandwidth;
    uint64_t rate;

    if (options == NULL) {
        return;
    }

    bandwidth = find_attribute(
        nla_data(options),
        (size_t)nla_len(options),
        TCA_CAKE_BASE_RATE64
    );
    if (read_u64(bandwidth, &rate)) {
        observation->bandwidth_bits_per_second =
            rate_to_bits_per_second(rate);
        observation->has_bandwidth = true;
    }
}

static void parse_basic_stats(
    const struct nlattr *basic,
    struct cake_observation *observation
)
{
    const unsigned char *data;
    size_t payload;

    if (basic == NULL) {
        return;
    }

    data = nla_data(basic);
    payload = (size_t)nla_len(basic);
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
    const struct nlattr *queue,
    struct cake_observation *observation
)
{
    struct gnet_stats_queue stats;

    if (queue == NULL || nla_len(queue) < (int)sizeof(stats)) {
        return;
    }

    memcpy(&stats, nla_data(queue), sizeof(stats));
    observation->queue_length = stats.qlen;
    observation->backlog_bytes = stats.backlog;
    observation->drops = stats.drops;
    observation->has_queue_stats = true;
}

static void parse_cake_stats(
    const struct nlattr *application,
    struct cake_observation *observation
)
{
    const struct nlattr *capacity;
    const struct nlattr *memory_limit;
    const struct nlattr *memory_used;
    uint64_t rate;
    bool has_memory_limit;
    bool has_memory_used;

    if (application == NULL) {
        return;
    }

    capacity = find_attribute(
        nla_data(application),
        (size_t)nla_len(application),
        TCA_CAKE_STATS_CAPACITY_ESTIMATE64
    );
    if (read_u64(capacity, &rate)) {
        observation->capacity_estimate_bits_per_second =
            rate_to_bits_per_second(rate);
        observation->has_capacity_estimate = true;
    }

    memory_limit = find_attribute(
        nla_data(application),
        (size_t)nla_len(application),
        TCA_CAKE_STATS_MEMORY_LIMIT
    );
    memory_used = find_attribute(
        nla_data(application),
        (size_t)nla_len(application),
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
    const struct nlattr *stats,
    struct cake_observation *observation
)
{
    if (stats == NULL) {
        return;
    }

    parse_basic_stats(
        find_attribute(
            nla_data(stats),
            (size_t)nla_len(stats),
            TCA_STATS_BASIC
        ),
        observation
    );
    parse_queue_stats(
        find_attribute(
            nla_data(stats),
            (size_t)nla_len(stats),
            TCA_STATS_QUEUE
        ),
        observation
    );
    parse_cake_stats(
        find_attribute(
            nla_data(stats),
            (size_t)nla_len(stats),
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
    const struct nlattr *attributes;
    const struct nlattr *kind;
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

    attributes = (const struct nlattr *)(
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
        error_set(
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
        error_set(error, error_size, "CAKE observation is null");
        return -1;
    }
    if (bandwidth_bits_per_second < 8U ||
        bandwidth_bits_per_second % 8U != 0U) {
        error_set(
            error,
            error_size,
            "CAKE bandwidth must be a positive multiple of 8 bit/s"
        );
        return -1;
    }

    errno = 0;
    interface_index = if_nametoindex(interface);
    if (interface_index == 0U) {
        error_set(
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
