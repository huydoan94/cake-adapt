#define _GNU_SOURCE

/* Exercise the actual parser with synthetic kernel messages, without a VM. */
#include "cake/cake.c"

#include <assert.h>
#include <netlink/msg.h>
#include <stdio.h>

static void test_qdisc_message(void)
{
	struct nl_msg *message = nlmsg_alloc_simple(RTM_NEWQDISC, 0);
	struct tcmsg tc = { .tcm_ifindex = 7, .tcm_parent = TC_H_ROOT, .tcm_handle = 0x10000U };
	struct cake_observation observation = { 0 };
	struct cake_read read = { .observation = &observation, .interface_index = 7U };
	struct cake_read_context context = { .reads = &read, .count = 1U };
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
	assert(nla_put_u32(message, TCA_CAKE_RAW, 0U) == 0);
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
	assert(read.found);
	assert(observation.has_bandwidth);
	assert(observation.raw);
	assert(observation.bandwidth_bits_per_second == 10000000U);
	assert(observation.has_basic_stats);
	assert(observation.bytes == bytes);
	assert(observation.packets == packets);
	assert(observation.queue_length == queue.qlen);
	assert(observation.backlog_bytes == queue.backlog);
	assert(observation.drops == queue.drops);
	assert(observation.capacity_estimate_bits_per_second == 10000000U);
	assert(observation.memory_limit_bytes == 8192U);
	assert(observation.memory_used_bytes == 1024U);

	read.found = false;
	read.interface_index = 8U;
	assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
	assert(!read.found);
	read.interface_index = 7U;
	((struct tcmsg *)NLMSG_DATA(nlmsg_hdr(message)))->tcm_parent = 0x10001U;
	assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
	assert(!read.found);
	nlmsg_free(message);
}

/* One dump carries every interface; each root qdisc fills only its own read. */
static void test_dump_routes_each_interface(void)
{
	struct nl_msg *messages[] = { nlmsg_alloc_simple(RTM_NEWQDISC, 0),
				      nlmsg_alloc_simple(RTM_NEWQDISC, 0),
				      nlmsg_alloc_simple(RTM_NEWQDISC, 0) };
	struct tcmsg qdiscs[] = {
		{ .tcm_ifindex = 8, .tcm_parent = TC_H_ROOT, .tcm_handle = 0x20000U },
		{ .tcm_ifindex = 7, .tcm_parent = TC_H_ROOT, .tcm_handle = 0x10000U },
		{ .tcm_ifindex = 7, .tcm_parent = TC_H_ROOT, .tcm_handle = 0x30000U }
	};
	struct cake_observation observations[2] = { { 0 } };
	struct cake_read reads[2] = { { .observation = &observations[0], .interface_index = 7U },
				      { .observation = &observations[1], .interface_index = 8U } };
	struct cake_read_context context = { .reads = reads, .count = 2U };
	size_t index;

	for (index = 0U; index < 3U; index++) {
		assert(messages[index] != NULL);
		assert(nlmsg_append(
			       messages[index],
			       &qdiscs[index],
			       sizeof(qdiscs[index]),
			       NLMSG_ALIGNTO
		       ) == 0);
		assert(nla_put_string(messages[index], TCA_KIND, "cake") == 0);
		assert(handle_qdisc(nlmsg_hdr(messages[index]), &context) == 0);
		nlmsg_free(messages[index]);
	}
	/* The first matching root wins; a later message cannot overwrite it. */
	assert(reads[0].found && observations[0].handle == 0x10000U);
	assert(observations[0].interface_index == 7U);
	assert(reads[1].found && observations[1].handle == 0x20000U);
	assert(observations[1].interface_index == 8U);
}

static void test_invalid_optional_attributes(void)
{
	struct nl_msg *message = nlmsg_alloc_simple(RTM_NEWQDISC, 0);
	struct tcmsg tc = { .tcm_ifindex = 7, .tcm_parent = TC_H_ROOT };
	struct cake_observation observation = { 0 };
	struct cake_read read = { .observation = &observation, .interface_index = 7U };
	struct cake_read_context context = { .reads = &read, .count = 1U };
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
	assert(read.found);
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
	assert(observation.capacity_estimate_bits_per_second == UINT64_MAX);
	assert(observation.memory_limit_bytes == 8192U);
	assert(observation.memory_used_bytes == 0U);
	nlmsg_free(message);
}

static void test_invalid_message(void)
{
	struct nl_msg *message = nlmsg_alloc_simple(RTM_NEWQDISC, 0);
	struct tcmsg tc = { .tcm_ifindex = 7, .tcm_parent = TC_H_ROOT };
	struct cake_observation observation = { 0 };
	struct cake_read read = { .observation = &observation, .interface_index = 7U };
	struct cake_read_context context = { .reads = &read, .count = 1U };
	struct nlmsghdr short_message = { .nlmsg_len = NLMSG_HDRLEN };

	assert(handle_qdisc(&short_message, &context) == -1);
	assert(message != NULL);
	assert(nlmsg_append(message, &tc, sizeof(tc), NLMSG_ALIGNTO) == 0);
	assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
	assert(!read.found);
	/* The kind must be a complete NUL-terminated string. */
	assert(nla_put(message, TCA_KIND, 4, "cake") == 0);
	assert(handle_qdisc(nlmsg_hdr(message), &context) == 0);
	assert(!read.found);
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
	assert(observation.queue_length == 0U);
	assert(observation.backlog_bytes == 0U);
	assert(observation.drops == 0U);

	nested = nla_nest_start(message, TCA_STATS_APP);
	assert(nested != NULL);
	assert(nla_put(message, TCA_CAKE_STATS_MEMORY_USED, 1, "x") == 0);
	assert(nla_nest_end(message, nested) == 0);
	parse_cake_stats(nested, &observation);
	assert(observation.memory_limit_bytes == 0U);
	assert(observation.memory_used_bytes == 0U);
	nlmsg_free(message);
}

static void test_missing_interface(void)
{
	struct netlink netlink = { 0 };
	struct cake_observation observation = { 0 };
	char error[128];
	/* Longer than IFNAMSIZ, so it cannot accidentally name a host interface. */
	const char *interface = "missing-interface";

	assert(cake_read(&netlink, interface, &observation, error, sizeof(error)) ==
	       CAKE_READ_ERROR);
	assert(strstr(error, "could not find interface") != NULL);
	assert(netlink.socket == NULL);
	assert(observation.interface_index == 0U);
}

/* Unprivileged qdisc dump against the host kernel's loopback, which has no CAKE. */
static void test_missing_cake_clears_cached_interface(void)
{
	struct netlink netlink = { 0 };
	struct cake_observation observation = { .interface_index = (unsigned int)INT_MAX,
						.has_mtu = true };
	char error[128] = "";

	/* A stale index cannot match any qdisc, so the next read resolves the name. */
	assert(cake_read(&netlink, "lo", &observation, error, sizeof(error)) ==
	       CAKE_READ_NOT_FOUND);
	assert(observation.interface_index == 0U);
	assert(!observation.has_mtu);
	assert(cake_read(&netlink, "lo", &observation, error, sizeof(error)) ==
	       CAKE_READ_NOT_FOUND);
	assert(observation.interface_index == 0U);
	netlink_close(&netlink);
}

/* A missing name fails alone; the other interface still gets its dump result. */
static void test_read_all_reports_each_interface(void)
{
	struct netlink netlink = { 0 };
	struct cake_observation observations[2] = { { 0 } };
	struct cake_read reads[2] = { { .interface = "missing-interface",
					.observation = &observations[0] },
				      { .interface = "lo", .observation = &observations[1] } };

	cake_read_all(&netlink, reads, 2U);
	assert(reads[0].result == CAKE_READ_ERROR);
	assert(strstr(reads[0].error, "could not find interface") != NULL);
	assert(reads[1].result == CAKE_READ_NOT_FOUND);
	assert(reads[1].error[0] == '\0');
	assert(netlink.socket != NULL);
	netlink_close(&netlink);
}

static void test_wire_packet_formula(void)
{
	struct cake_observation observation = { .mtu_bytes = 1500U, .has_mtu = true };

	observation.atm_mode = CAKE_ATM_NONE;
	observation.overhead_bytes = 44;
	assert(cake_max_wire_packet_bits(&observation) == 12352U);
	observation.atm_mode = CAKE_ATM_ATM;
	assert(cake_max_wire_packet_bits(&observation) == 13992U);
	observation.atm_mode = CAKE_ATM_PTM;
	assert(cake_max_wire_packet_bits(&observation) == 12000U);
	observation.atm_mode = CAKE_ATM_ATM;
	observation.raw = true;
	assert(cake_max_wire_packet_bits(&observation) == 12000U);
	observation.raw = false;
	observation.overhead_bytes = -1;
	assert(cake_max_wire_packet_bits(&observation) == 12000U);
}

int main(void)
{
	test_qdisc_message();
	test_dump_routes_each_interface();
	test_invalid_optional_attributes();
	test_optional_application_stats();
	test_invalid_message();
	test_short_queue_and_memory();
	test_missing_interface();
	test_missing_cake_clears_cached_interface();
	test_read_all_reports_each_interface();
	test_wire_packet_formula();
	(void)puts("cake parser tests passed");
	return 0;
}
