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
#include <netlink/msg.h>
#include <string.h>

struct cake_dump_context {
    unsigned int interface_index;
    struct cake_observation *observation;
    bool found;
};

static uint64_t rate_to_bits_per_second(uint64_t bytes_per_second)
{
    uint64_t bits_per_second;

    if (
        __builtin_mul_overflow(
            bytes_per_second,
            UINT64_C(8),
            &bits_per_second
        )
    ) {
        return UINT64_MAX;
    }
    return bits_per_second;
}

static void parse_options(
    struct nlattr *options,
    struct cake_observation *observation
)
{
    static const struct nla_policy policy[TCA_CAKE_MAX + 1] = {
        [TCA_CAKE_BASE_RATE64] = { .type = NLA_U64 }
    };
    struct nlattr *attributes[TCA_CAKE_MAX + 1];

    if (options == NULL ||
        nla_parse_nested(
            attributes,
            TCA_CAKE_MAX,
            options,
            policy
        ) < 0) {
        return;
    }
    if (attributes[TCA_CAKE_BASE_RATE64] != NULL) {
        observation->bandwidth_bits_per_second =
            rate_to_bits_per_second(nla_get_u64(attributes[TCA_CAKE_BASE_RATE64]));
        observation->has_bandwidth = true;
    }
}

static void parse_cake_stats(
    struct nlattr *application,
    struct cake_observation *observation
)
{
    static const struct nla_policy policy[TCA_CAKE_STATS_MAX + 1] = {
        [TCA_CAKE_STATS_CAPACITY_ESTIMATE64] = { .type = NLA_U64 },
        [TCA_CAKE_STATS_MEMORY_LIMIT] = { .type = NLA_U32 },
        [TCA_CAKE_STATS_MEMORY_USED] = { .type = NLA_U32 }
    };
    struct nlattr *attributes[TCA_CAKE_STATS_MAX + 1];

    if (application == NULL ||
        nla_parse_nested(
            attributes,
            TCA_CAKE_STATS_MAX,
            application,
            policy
        ) < 0) {
        return;
    }
    if (attributes[TCA_CAKE_STATS_CAPACITY_ESTIMATE64] != NULL) {
        observation->capacity_estimate_bits_per_second =
            rate_to_bits_per_second(nla_get_u64(attributes[TCA_CAKE_STATS_CAPACITY_ESTIMATE64]));
        observation->has_capacity_estimate = true;
    }
    if (attributes[TCA_CAKE_STATS_MEMORY_LIMIT] != NULL) {
        observation->memory_limit_bytes = nla_get_u32(attributes[TCA_CAKE_STATS_MEMORY_LIMIT]);
    }
    if (attributes[TCA_CAKE_STATS_MEMORY_USED] != NULL) {
        observation->memory_used_bytes = nla_get_u32(attributes[TCA_CAKE_STATS_MEMORY_USED]);
    }
    observation->has_memory_stats =
        attributes[TCA_CAKE_STATS_MEMORY_LIMIT] != NULL &&
        attributes[TCA_CAKE_STATS_MEMORY_USED] != NULL;
}

static void parse_stats(
    struct nlattr *stats,
    struct cake_observation *observation
)
{
    static const struct nla_policy policy[TCA_STATS_MAX + 1] = {
        /* The ABI is 12 bytes; sizeof(gnet_stats_basic) can include padding. */
        [TCA_STATS_BASIC] = {
            .type = NLA_BINARY,
            .minlen = sizeof(uint64_t) + sizeof(uint32_t)
        },
        [TCA_STATS_QUEUE] = {
            .type = NLA_BINARY,
            .minlen = sizeof(struct gnet_stats_queue)
        }
    };
    struct nlattr *attributes[TCA_STATS_MAX + 1];

    /* Invalid optional statistics do not prevent discovering the qdisc. */
    if (stats == NULL ||
        nla_parse_nested(
            attributes,
            TCA_STATS_MAX,
            stats,
            policy
        ) < 0) {
        return;
    }
    if (attributes[TCA_STATS_BASIC] != NULL) {
        struct gnet_stats_basic basic = { 0 };

        nla_memcpy(&basic, attributes[TCA_STATS_BASIC], sizeof(basic));
        observation->bytes = basic.bytes;
        observation->packets = basic.packets;
        observation->has_basic_stats = true;
    }
    if (attributes[TCA_STATS_QUEUE] != NULL) {
        struct gnet_stats_queue queue;

        nla_memcpy(&queue, attributes[TCA_STATS_QUEUE], sizeof(queue));
        observation->queue_length = queue.qlen;
        observation->backlog_bytes = queue.backlog;
        observation->drops = queue.drops;
        observation->has_queue_stats = true;
    }
    parse_cake_stats(attributes[TCA_STATS_APP], observation);
}

static int handle_qdisc(
    const struct nlmsghdr *message,
    void *context_pointer
)
{
    struct cake_dump_context *context = context_pointer;
    static const struct nla_policy policy[TCA_MAX + 1] = {
        [TCA_KIND] = { .type = NLA_NUL_STRING }
    };
    const struct tcmsg *traffic_control;
    struct nlattr *attributes[TCA_MAX + 1];

    if (!nlmsg_valid_hdr(message, sizeof(struct tcmsg)) ||
        message->nlmsg_len > INT_MAX) {
        return -1;
    }

    traffic_control = NLMSG_DATA(message);
    /* Only control the interface's root CAKE, never a nested child qdisc. */
    if (traffic_control->tcm_ifindex != (int)context->interface_index ||
        traffic_control->tcm_parent != TC_H_ROOT ||
        context->found) {
        return 0;
    }

    if (nla_parse(
            attributes,
            TCA_MAX,
            nlmsg_attrdata(message, sizeof(*traffic_control)),
            nlmsg_attrlen(message, sizeof(*traffic_control)),
            policy
        ) < 0 ||
        attributes[TCA_KIND] == NULL ||
        nla_strcmp(attributes[TCA_KIND], "cake") != 0) {
        return 0;
    }

    memset(context->observation, 0, sizeof(*context->observation));
    context->observation->handle = traffic_control->tcm_handle;
    context->observation->parent = traffic_control->tcm_parent;
    parse_options(
        attributes[TCA_OPTIONS],
        context->observation
    );
    parse_stats(
        attributes[TCA_STATS2],
        context->observation
    );
    context->found = true;
    return 0;
}

enum cake_read_result cake_read(
    struct netlink *netlink,
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
        netlink_close_requests(netlink);
        return CAKE_READ_ERROR;
    }

    return context.found ? CAKE_READ_FOUND : CAKE_READ_NOT_FOUND;
}

int cake_set_bandwidth(
    struct netlink *netlink,
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
        netlink_close_requests(netlink);
        return -1;
    }

    return 0;
}
