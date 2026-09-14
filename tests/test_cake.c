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
    struct nlattr *nested;

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
    assert(nla_nest_end(message, nested) == 0);

    assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
    assert(context.found);
    assert(observation.has_bandwidth);
    assert(observation.bandwidth_bits_per_second == 10000000U);
    assert(observation.has_basic_stats);
    assert(observation.bytes == bytes);
    assert(observation.packets == packets);

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

static void test_truncated_attributes(void)
{
    struct nlattr attribute = { .nla_len = sizeof(attribute) - 1U, .nla_type = TCA_KIND };
    struct cake_observation observation = { 0 };
    struct cake_dump_context context = { .observation = &observation };
    struct nlmsghdr message = { .nlmsg_len = NLMSG_HDRLEN };
    uint64_t value;

    assert(find_attribute(&attribute, sizeof(attribute), TCA_KIND) == NULL);
    attribute.nla_len = sizeof(attribute);
    assert(!read_u64(&attribute, &value));
    assert(!read_u64(NULL, &value));
    assert(handle_qdisc(&message, &context) == -1);
}

int main(void)
{
    test_qdisc_message();
    test_truncated_attributes();
    (void)puts("cake parser tests passed");
    return 0;
}
