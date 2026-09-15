#define _GNU_SOURCE

/* Exercise the actual parser with synthetic kernel messages, without a VM. */
#include "../src/cake.c"

#include <assert.h>
#include <netlink/msg.h>
#include <stdio.h>

static void test_qdisc_message(void)
{
    struct nl_msg *message = nlmsg_alloc_simple(RTM_NEWQDISC, 0);
    struct tcmsg tc = {
        .tcm_ifindex = 7,
        .tcm_parent = TC_H_ROOT,
        .tcm_handle = 0x10000U
    };
    struct cake_observation observation = { 0 };
    struct cake_dump_context context = {
        .interface_index = 7U,
        .observation = &observation
    };
    uint64_t bandwidth = 1250000U;
    uint64_t bytes = UINT64_C(5000000000);
    uint32_t packets = 123U;
    unsigned char basic[sizeof(bytes) + sizeof(packets)];
    struct gnet_stats_queue queue = { .qlen = 3U, .backlog = 4096U, .drops = 5U };
    struct nlattr *nested;
    struct nlattr *application;

    assert(message != NULL);
    assert(nlmsg_append(message, &tc, sizeof(tc), NLMSG_ALIGNTO) == 0);
    assert(nla_put_string(message, TCA_KIND, "cake") == 0);
    nested = nla_nest_start(message, TCA_OPTIONS);
    assert(nested != NULL);
    assert(nla_put(message, TCA_CAKE_BASE_RATE64, sizeof(bandwidth), &bandwidth) == 0);
    assert(nla_nest_end(message, nested) == 0);
    memcpy(basic, &bytes, sizeof(bytes));
    memcpy(basic + sizeof(bytes), &packets, sizeof(packets));
    nested = nla_nest_start(message, TCA_STATS2);
    assert(nested != NULL);
    assert(nla_put(message, TCA_STATS_BASIC, sizeof(basic), basic) == 0);
    assert(nla_put(message, TCA_STATS_QUEUE, sizeof(queue), &queue) == 0);
    application = nla_nest_start(message, TCA_STATS_APP);
    assert(application != NULL);
    assert(nla_put_u64(message, TCA_CAKE_STATS_CAPACITY_ESTIMATE64, bandwidth) == 0);
    assert(nla_put_u32(message, TCA_CAKE_STATS_MEMORY_LIMIT, 8192U) == 0);
    assert(nla_put_u32(message, TCA_CAKE_STATS_MEMORY_USED, 1024U) == 0);
    /* New kernel attributes must not break decoding known fields. */
    assert(nla_put_u32(message, TCA_CAKE_STATS_MAX + 1, 42U) == 0);
    assert(nla_nest_end(message, application) == 0);
    assert(nla_nest_end(message, nested) == 0);

    assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
    assert(context.found);
    assert(observation.has_bandwidth);
    assert(observation.bandwidth_bits_per_second == 10000000U);
    assert(observation.has_basic_stats);
    assert(observation.bytes == bytes);
    assert(observation.packets == packets);
    assert(observation.has_queue_stats);
    assert(observation.queue_length == queue.qlen);
    assert(observation.backlog_bytes == queue.backlog);
    assert(observation.drops == queue.drops);
    assert(observation.has_capacity_estimate);
    assert(observation.capacity_estimate_bits_per_second == 10000000U);
    assert(observation.has_memory_stats);
    assert(observation.memory_limit_bytes == 8192U);
    assert(observation.memory_used_bytes == 1024U);

    context.found = false;
    context.interface_index = 8U;
    assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
    assert(!context.found);
    context.interface_index = 7U;
    ((struct tcmsg *)NLMSG_DATA(nlmsg_hdr(message)))->tcm_parent = 0x10001U;
    assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
    assert(!context.found);
    nlmsg_free(message);
}

static void test_invalid_optional_attributes(void)
{
    struct nl_msg *message = nlmsg_alloc_simple(RTM_NEWQDISC, 0);
    struct tcmsg tc = { .tcm_ifindex = 7, .tcm_parent = TC_H_ROOT };
    struct cake_observation observation = { 0 };
    struct cake_dump_context context = {
        .interface_index = 7U,
        .observation = &observation
    };
    struct nlattr *nested;

    assert(message != NULL);
    assert(nlmsg_append(message, &tc, sizeof(tc), NLMSG_ALIGNTO) == 0);
    assert(nla_put_string(message, TCA_KIND, "cake") == 0);
    nested = nla_nest_start(message, TCA_OPTIONS);
    assert(nested != NULL);
    /* A truncated u64 must never reach nla_get_u64. */
    assert(nla_put_u32(message, TCA_CAKE_BASE_RATE64, 1U) == 0);
    assert(nla_nest_end(message, nested) == 0);
    nested = nla_nest_start(message, TCA_STATS2);
    assert(nested != NULL);
    assert(nla_put_u32(message, TCA_STATS_BASIC, 1U) == 0);
    assert(nla_nest_end(message, nested) == 0);
    assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
    assert(context.found);
    assert(!observation.has_bandwidth);
    assert(!observation.has_basic_stats);
    nlmsg_free(message);
}

static void test_optional_application_stats(void)
{
    struct nl_msg *message = nlmsg_alloc();
    struct cake_observation observation = { 0 };
    struct nlattr *application;

    assert(message != NULL);
    application = nla_nest_start(message, TCA_STATS_APP);
    assert(application != NULL);
    assert(nla_put_u32(message, TCA_CAKE_STATS_MEMORY_LIMIT, 8192U) == 0);
    assert(nla_put_u64(message, TCA_CAKE_STATS_CAPACITY_ESTIMATE64, UINT64_MAX) == 0);
    assert(nla_nest_end(message, application) == 0);
    parse_cake_stats(application, &observation);
    assert(observation.has_capacity_estimate);
    assert(observation.capacity_estimate_bits_per_second == UINT64_MAX);
    assert(!observation.has_memory_stats);
    assert(observation.memory_limit_bytes == 8192U);
    nlmsg_free(message);
}

static void test_invalid_message(void)
{
    struct nl_msg *message = nlmsg_alloc_simple(RTM_NEWQDISC, 0);
    struct tcmsg tc = { .tcm_ifindex = 7, .tcm_parent = TC_H_ROOT };
    struct cake_observation observation = { 0 };
    struct cake_dump_context context = {
        .interface_index = 7U,
        .observation = &observation
    };
    struct nlmsghdr short_message = { .nlmsg_len = NLMSG_HDRLEN };

    assert(handle_qdisc(&short_message, &context) == -1);
    assert(message != NULL);
    assert(nlmsg_append(message, &tc, sizeof(tc), NLMSG_ALIGNTO) == 0);
    assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
    assert(!context.found);
    /* The kind must be a complete NUL-terminated string. */
    assert(nla_put(message, TCA_KIND, 4, "cake") == 0);
    assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
    assert(!context.found);
    nlmsg_free(message);
}

static void test_short_queue_and_memory(void)
{
    struct nl_msg *message = nlmsg_alloc();
    struct cake_observation observation = { 0 };
    struct nlattr *nested;

    assert(message != NULL);
    nested = nla_nest_start(message, TCA_STATS2);
    assert(nested != NULL);
    assert(nla_put_u32(message, TCA_STATS_QUEUE, 1U) == 0);
    assert(nla_nest_end(message, nested) == 0);
    parse_stats(nested, &observation);
    assert(!observation.has_queue_stats);

    nested = nla_nest_start(message, TCA_STATS_APP);
    assert(nested != NULL);
    assert(nla_put(message, TCA_CAKE_STATS_MEMORY_USED, 1, "x") == 0);
    assert(nla_nest_end(message, nested) == 0);
    parse_cake_stats(nested, &observation);
    assert(!observation.has_memory_stats);
    assert(observation.memory_used_bytes == 0U);
    nlmsg_free(message);
}

int main(void)
{
    test_qdisc_message();
    test_invalid_optional_attributes();
    test_optional_application_stats();
    test_invalid_message();
    test_short_queue_and_memory();
    (void)puts("cake parser tests passed");
    return 0;
}
