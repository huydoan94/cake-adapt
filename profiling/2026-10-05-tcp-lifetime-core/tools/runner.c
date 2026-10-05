/* SPDX-License-Identifier: MIT */
/* Target-kernel acceptance runner for the TCP lifetime socket filter. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/if.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "tcpdelay/estimator.h"

#define TCP_TIMESTAMP_OPTION_BYTES 12U
#define TCP_HEADER_BYTES (sizeof(struct tcphdr) + TCP_TIMESTAMP_OPTION_BYTES)
#define IPV4_HEADER_BYTES sizeof(struct iphdr)
#define ETHERNET_HEADER_BYTES sizeof(struct ethhdr)
#define PACKET_BUFFER_BYTES (ETHERNET_HEADER_BYTES + IPV4_HEADER_BYTES + TCP_HEADER_BYTES)
#define MAP_COUNT 10U
#define MAX_RING_RECORDS 256U
#define GENERATION_FIRST 1U
#define GENERATION_TERMINAL UINT32_MAX
#define ACK_SEQUENCE_ADVANCE 1U
#define TSECR_OFFSET 8U
#define TSVAL_OFFSET 4U
#define BPF_RECORD_SIZE 64U
#define TCP_FLAG_SYN 0x02U
#define TCP_FLAG_ACK 0x10U
#define SIDE_LOCAL 0U
#define SIDE_REMOTE 1U
#define IDENTITY_MASK_CONFIRMED 3U

/* Private map ABI mirrored from tcpdelay.bpf.c; runtime map-info checks below. */
struct flow {
	uint8_t local_address[16];
	uint8_t remote_address[16];
	uint16_t local_port;
	uint16_t remote_port;
};

struct departure_key {
	struct flow flow;
	uint32_t generation;
	uint32_t tsval;
};

struct generation_flow_key {
	struct flow flow;
	uint32_t generation;
};

struct side_key {
	struct flow flow;
	uint32_t side;
};

struct seen_syn_key {
	struct flow flow;
	uint32_t side;
	uint32_t isn;
};

struct confirmed_pair_key {
	struct flow flow;
	uint32_t local_isn;
	uint32_t remote_isn;
};

struct identity_key {
	struct flow flow;
	uint32_t generation;
};

struct connection_identity {
	uint32_t local_isn;
	uint32_t remote_isn;
	uint8_t mask;
	uint8_t reserved[3];
};

struct connection_lifecycle {
	uint32_t active_generation;
	uint32_t latest_generation;
};

struct counter_values {
	uint64_t ring_full;
	uint64_t generation_failures;
	uint64_t ack_bytes;
	uint64_t upload_bytes;
};

struct record {
	uint64_t arrival_ns;
	uint64_t departure_ns;
	struct flow flow;
	uint32_t tsval;
	uint32_t generation;
	uint32_t reserved;
};

_Static_assert(sizeof(struct flow) == 36U, "flow key ABI");
_Static_assert(sizeof(struct departure_key) == 44U, "departure key ABI");
_Static_assert(sizeof(struct generation_flow_key) == 40U, "generation-state key ABI");
_Static_assert(sizeof(struct side_key) == 40U, "pending-side key ABI");
_Static_assert(sizeof(struct seen_syn_key) == 44U, "seen-SYN key ABI");
_Static_assert(sizeof(struct confirmed_pair_key) == 44U, "confirmed-pair key ABI");
_Static_assert(sizeof(struct identity_key) == 40U, "identity key ABI");
_Static_assert(sizeof(struct connection_identity) == 12U, "identity value ABI");
_Static_assert(sizeof(struct connection_lifecycle) == 8U, "lifecycle value ABI");
_Static_assert(sizeof(struct counter_values) == 32U, "counter value ABI");
_Static_assert(sizeof(struct record) == BPF_RECORD_SIZE, "ring record ABI");
_Static_assert(offsetof(struct record, generation) == 56U, "ring generation offset ABI");
_Static_assert(sizeof(struct tcpdelay_record_flow) == sizeof(struct flow), "estimator flow ABI");

struct map_expectation {
	const char *name;
	enum bpf_map_type type;
	uint32_t key_size;
	uint32_t value_size;
	uint32_t max_entries;
	uint32_t map_flags;
};

struct harness {
	struct bpf_object *object;
	struct ring_buffer *ring;
	struct tcpdelay_estimator estimator;
	struct record records[MAX_RING_RECORDS];
	size_t record_count;
	int filter_socket;
	int transmit_socket;
	int program_fd;
	int map_fds[MAP_COUNT];
	unsigned int capture_ifindex;
	unsigned int peer_ifindex;
};

struct packet_spec {
	uint16_t local_port;
	uint32_t sequence;
	uint32_t acknowledgment;
	uint32_t tsval;
	uint32_t tsecr;
	uint8_t flags;
	int outgoing;
};

static const struct map_expectation expected_maps[MAP_COUNT] = {
	{ "seen_syn", BPF_MAP_TYPE_LRU_HASH, sizeof(struct seen_syn_key), 1U, 4096U, 0U },
	{ "pending_sides", BPF_MAP_TYPE_LRU_HASH, sizeof(struct side_key), sizeof(uint32_t), 2048U, 0U },
	{ "lifecycles", BPF_MAP_TYPE_LRU_HASH, sizeof(struct flow), sizeof(struct connection_lifecycle), 1024U, 0U },
	{ "confirmed_pairs", BPF_MAP_TYPE_LRU_HASH, sizeof(struct confirmed_pair_key), sizeof(uint32_t), 4096U, 0U },
	{ "identities", BPF_MAP_TYPE_LRU_HASH, sizeof(struct identity_key), sizeof(struct connection_identity), 1024U, 0U },
	{ "generation_states", BPF_MAP_TYPE_LRU_HASH, sizeof(struct generation_flow_key), 32U, 1024U, 0U },
	{ "departures", BPF_MAP_TYPE_LRU_HASH, sizeof(struct departure_key), sizeof(uint64_t), 8192U, 0U },
	{ "generation_allocator", BPF_MAP_TYPE_ARRAY, sizeof(uint32_t), sizeof(uint32_t), 1U, 0U },
	{ "samples", BPF_MAP_TYPE_RINGBUF, 0U, 0U, 256U * 1024U, 0U },
	{ "counters", BPF_MAP_TYPE_PERCPU_ARRAY, sizeof(uint32_t), sizeof(struct counter_values), 1U, 0U },
};

static unsigned int assertions;
static unsigned int failures;

static void check(int condition, const char *name)
{
	assertions++;
	printf("ASSERT\t%s\t%s\n", condition ? "PASS" : "FAIL", name);
	if (!condition)
		failures++;
}

static void fail_errno(const char *operation)
{
	fprintf(stderr, "%s: %s\n", operation, strerror(errno));
	exit(EXIT_FAILURE);
}

static int ring_record(void *context, void *data, size_t size)
{
	struct harness *harness = context;
	const struct record *record = data;
	struct tcpdelay_sample sample;

	if (size != sizeof(*record)) {
		fprintf(stderr, "ring record size %zu != %zu\n", size, sizeof(*record));
		return -1;
	}
	if (harness->record_count == MAX_RING_RECORDS) {
		fprintf(stderr, "test ring record buffer full\n");
		return -1;
	}
	harness->records[harness->record_count++] = *record;
	memcpy(&sample.flow, &record->flow, sizeof(sample.flow));
	sample.arrival_ns = record->arrival_ns;
	sample.departure_ns = record->departure_ns;
	sample.tsval = record->tsval;
	sample.generation = record->generation;
	tcpdelay_estimator_add(&harness->estimator, &sample);
	return 0;
}

static int map_fd(const struct harness *harness, const char *name)
{
	return bpf_object__find_map_fd_by_name(harness->object, name);
}

static void verify_map_abi(struct harness *harness)
{
	size_t index;

	for (index = 0U; index < MAP_COUNT; index++) {
		const struct map_expectation *expected = &expected_maps[index];
		struct bpf_map_info info = { 0 };
		uint32_t info_size = sizeof(info);
		struct bpf_map *map = bpf_object__find_map_by_name(harness->object, expected->name);
		int descriptor = map == NULL ? -1 : bpf_map__fd(map);
		char assertion[128];
		int valid;

		if (descriptor < 0)
			fail_errno("find map descriptor");
		harness->map_fds[index] = descriptor;
		if (bpf_map_get_info_by_fd(descriptor, &info, &info_size) != 0)
			fail_errno("query map ABI");
		valid = info.type == (uint32_t)expected->type &&
			info.key_size == expected->key_size &&
			info.value_size == expected->value_size &&
			info.max_entries == expected->max_entries &&
			info.map_flags == expected->map_flags;
		(void)snprintf(assertion, sizeof(assertion), "map_abi_%s", expected->name);
	printf("MAP\t%s\tkernel_name=%.*s\ttype=%u\tkey=%u\tvalue=%u\tcapacity=%u\tflags=%u\n",
		       expected->name,
		       BPF_OBJ_NAME_LEN,
		       info.name,
		       info.type,
		       info.key_size,
		       info.value_size,
		       info.max_entries,
		       info.map_flags);
		check(valid, assertion);
	}
}

static void report_fdinfo(const char *kind, const char *name, int descriptor)
{
	FILE *fdinfo;
	char path[64];
	char line[256];

	(void)snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", descriptor);
	fdinfo = fopen(path, "r");
	if (fdinfo == NULL) {
		printf("FDINFO\t%s\t%s\tunavailable\terrno=%d\n", kind, name, errno);
		return;
	}
	while (fgets(line, sizeof(line), fdinfo) != NULL)
		printf("FDINFO\t%s\t%s\t%s", kind, name, line);
	(void)fclose(fdinfo);
}

static void report_map_accounting(const struct harness *harness)
{
	size_t index;

	for (index = 0U; index < MAP_COUNT; index++)
		report_fdinfo("map", expected_maps[index].name, harness->map_fds[index]);
}

static void initialize_harness(
	struct harness *harness,
	const char *object_path,
	const char *capture_interface,
	const char *peer_interface
)
{
	struct bpf_program *program;
	struct bpf_object_open_opts open_options = { .sz = sizeof(open_options) };
	char *verifier_log = calloc(1024U * 1024U, sizeof(*verifier_log));
	uint32_t allocator_key = 0U;
	uint32_t initial_generation = GENERATION_FIRST;
	struct sockaddr_ll address = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL) };

	memset(harness, 0, sizeof(*harness));
	harness->filter_socket = -1;
	harness->transmit_socket = -1;
	harness->capture_ifindex = if_nametoindex(capture_interface);
	harness->peer_ifindex = if_nametoindex(peer_interface);
	if (harness->capture_ifindex == 0U || harness->peer_ifindex == 0U)
		fail_errno("resolve veth interface");
	if (verifier_log == NULL)
		fail_errno("allocate verifier log buffer");
	tcpdelay_estimator_init(&harness->estimator);
	open_options.kernel_log_buf = verifier_log;
	open_options.kernel_log_size = 1024U * 1024U;
	open_options.kernel_log_level = 1U;
	harness->object = bpf_object__open_file(object_path, &open_options);
	if (harness->object == NULL)
		fail_errno("open BPF object");
	if (bpf_object__load(harness->object) != 0)
		fail_errno("load BPF object");
	printf("VERIFIER_LOG_BEGIN\n%s\nVERIFIER_LOG_END\n", verifier_log);
	check(verifier_log[0] != '\0', "successful_kernel_verifier_log_captured");
	free(verifier_log);
	verify_map_abi(harness);
	if (bpf_map_update_elem(map_fd(harness, "generation_allocator"),
				&allocator_key,
				&initial_generation,
				BPF_ANY) != 0)
		fail_errno("initialize generation allocator");
	program = bpf_object__find_program_by_name(harness->object, "tcpdelay");
	if (program == NULL)
		fail_errno("find tcpdelay program");
	harness->program_fd = bpf_program__fd(program);
	harness->ring = ring_buffer__new(map_fd(harness, "samples"), ring_record, harness, NULL);
	if (harness->ring == NULL)
		fail_errno("create ring buffer");
	harness->filter_socket = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (harness->filter_socket < 0)
		fail_errno("open capture packet socket");
	if (setsockopt(harness->filter_socket,
		       SOL_SOCKET,
		       SO_ATTACH_BPF,
		       &harness->program_fd,
		       sizeof(harness->program_fd)) != 0)
		fail_errno("attach exact filter");
	address.sll_ifindex = (int)harness->capture_ifindex;
	if (bind(harness->filter_socket, (struct sockaddr *)&address, sizeof(address)) != 0)
		fail_errno("bind capture packet socket");
	harness->transmit_socket = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (harness->transmit_socket < 0)
		fail_errno("open transmit packet socket");
	report_map_accounting(harness);
	report_fdinfo("program", "tcpdelay", harness->program_fd);
}

static struct flow flow_for(uint16_t port)
{
	struct flow flow = { 0 };
	struct in_addr local;
	struct in_addr remote;

	if (inet_pton(AF_INET, "192.0.2.1", &local) != 1 ||
	    inet_pton(AF_INET, "192.0.2.2", &remote) != 1)
		abort();
	flow.local_address[10] = 0xffU;
	flow.local_address[11] = 0xffU;
	flow.remote_address[10] = 0xffU;
	flow.remote_address[11] = 0xffU;
	memcpy(&flow.local_address[12], &local, sizeof(local));
	memcpy(&flow.remote_address[12], &remote, sizeof(remote));
	flow.local_port = htons(port);
	flow.remote_port = htons(443U);
	return flow;
}

static void build_packet(
	uint8_t packet[PACKET_BUFFER_BYTES],
	const struct packet_spec *spec
)
{
	struct ethhdr *ethernet = (struct ethhdr *)packet;
	struct iphdr *ip = (struct iphdr *)(packet + ETHERNET_HEADER_BYTES);
	struct tcphdr *tcp = (struct tcphdr *)(packet + ETHERNET_HEADER_BYTES + IPV4_HEADER_BYTES);
	uint8_t *options = packet + ETHERNET_HEADER_BYTES + IPV4_HEADER_BYTES + sizeof(*tcp);
	const uint8_t local_mac[ETH_ALEN] = { 0x02U, 0U, 0U, 0U, 0U, 1U };
	const uint8_t remote_mac[ETH_ALEN] = { 0x02U, 0U, 0U, 0U, 0U, 2U };
	struct in_addr local;
	struct in_addr remote;

	memset(packet, 0, PACKET_BUFFER_BYTES);
	if (inet_pton(AF_INET, "192.0.2.1", &local) != 1 ||
	    inet_pton(AF_INET, "192.0.2.2", &remote) != 1)
		abort();
	memcpy(ethernet->h_source, spec->outgoing ? local_mac : remote_mac, ETH_ALEN);
	memcpy(ethernet->h_dest, spec->outgoing ? remote_mac : local_mac, ETH_ALEN);
	ethernet->h_proto = htons(ETH_P_IP);
	ip->version = 4U;
	ip->ihl = 5U;
	ip->ttl = 64U;
	ip->protocol = IPPROTO_TCP;
	ip->tot_len = htons((uint16_t)(IPV4_HEADER_BYTES + TCP_HEADER_BYTES));
	ip->saddr = spec->outgoing ? local.s_addr : remote.s_addr;
	ip->daddr = spec->outgoing ? remote.s_addr : local.s_addr;
	tcp->source = spec->outgoing ? htons(spec->local_port) : htons(443U);
	tcp->dest = spec->outgoing ? htons(443U) : htons(spec->local_port);
	tcp->seq = htonl(spec->sequence);
	tcp->ack_seq = htonl(spec->acknowledgment);
	tcp->doff = (uint8_t)(TCP_HEADER_BYTES / 4U);
	tcp->ack = (spec->flags & TCP_FLAG_ACK) != 0U;
	tcp->syn = (spec->flags & TCP_FLAG_SYN) != 0U;
	tcp->fin = (spec->flags & 0x01U) != 0U;
	tcp->rst = (spec->flags & 0x04U) != 0U;
	options[0] = 1U;
	options[1] = 1U;
	options[2] = 8U;
	options[3] = 10U;
	memcpy(options + TSVAL_OFFSET, &(uint32_t){ htonl(spec->tsval) }, sizeof(uint32_t));
	memcpy(options + TSECR_OFFSET, &(uint32_t){ htonl(spec->tsecr) }, sizeof(uint32_t));
}

static void inject(struct harness *harness, const struct packet_spec *spec)
{
	uint8_t packet[PACKET_BUFFER_BYTES];
	struct sockaddr_ll address = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_IP),
		.sll_ifindex = (int)(spec->outgoing ? harness->capture_ifindex : harness->peer_ifindex),
		.sll_halen = ETH_ALEN,
	};

	build_packet(packet, spec);
	if (sendto(harness->transmit_socket,
		   packet,
		   sizeof(packet),
		   0,
		   (struct sockaddr *)&address,
		   sizeof(address)) != (ssize_t)sizeof(packet))
		fail_errno("inject timestamp packet");
	/* Let both packet taps and the BPF ring submission run before inspecting state. */
	{
		struct timespec pause = { .tv_nsec = 1000000L };
		(void)nanosleep(&pause, NULL);
	}
}

static int consume_ring(struct harness *harness)
{
	int result = ring_buffer__consume(harness->ring);

	if (result < 0) {
		fprintf(stderr, "ring_buffer__consume: %s\n", strerror(-result));
		return -1;
	}
	return result;
}

static size_t begin_scenario(struct harness *harness)
{
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	return harness->record_count;
}

static size_t records_for_flow(
	const struct harness *harness,
	size_t first,
	const struct flow *flow
)
{
	size_t index;
	size_t count = 0U;

	for (index = first; index < harness->record_count; index++)
		count += memcmp(&harness->records[index].flow, flow, sizeof(*flow)) == 0;
	return count;
}

static const struct record *find_record(
	const struct harness *harness,
	size_t first,
	const struct flow *flow,
	uint32_t generation,
	uint32_t tsval
)
{
	size_t index;

	for (index = first; index < harness->record_count; index++) {
		const struct record *record = &harness->records[index];

		if (record->generation == generation && record->tsval == tsval &&
		    memcmp(&record->flow, flow, sizeof(*flow)) == 0)
			return record;
	}
	return NULL;
}

static uint32_t lifecycle_generation(struct harness *harness, const struct flow *flow)
{
	struct connection_lifecycle lifecycle;
	int descriptor = map_fd(harness, "lifecycles");

	if (bpf_map_lookup_elem(descriptor, flow, &lifecycle) != 0)
		return 0U;
	return lifecycle.active_generation;
}

static int handshake_state_valid(
	struct harness *harness,
	const struct flow *flow,
	uint32_t local_isn,
	uint32_t remote_isn,
	uint32_t generation,
	uint32_t local_tsval
)
{
	struct connection_lifecycle lifecycle;
	struct confirmed_pair_key pair = { .flow = *flow, .local_isn = local_isn,
					   .remote_isn = remote_isn };
	struct identity_key identity_key = { .flow = *flow, .generation = generation };
	struct generation_flow_key state_key = { .flow = *flow, .generation = generation };
	struct departure_key departure_key = { .flow = *flow, .generation = generation,
					       .tsval = local_tsval };
	struct side_key local_side = { .flow = *flow, .side = SIDE_LOCAL };
	struct side_key remote_side = { .flow = *flow, .side = SIDE_REMOTE };
	struct connection_identity found_identity;
	uint32_t active_pair_generation;
	uint32_t local_pending;
	uint32_t remote_pending;
	uint32_t state_bytes[8];
	uint64_t departure_ns;
	int valid;

	valid = bpf_map_lookup_elem(map_fd(harness, "lifecycles"), flow, &lifecycle) == 0 &&
		lifecycle.active_generation == generation && lifecycle.latest_generation == generation &&
		bpf_map_lookup_elem(map_fd(harness, "pending_sides"), &local_side, &local_pending) == 0 &&
		local_pending == local_isn &&
		bpf_map_lookup_elem(map_fd(harness, "pending_sides"), &remote_side, &remote_pending) == 0 &&
		remote_pending == remote_isn &&
		bpf_map_lookup_elem(map_fd(harness, "confirmed_pairs"), &pair, &active_pair_generation) == 0 &&
		active_pair_generation == generation &&
		bpf_map_lookup_elem(map_fd(harness, "identities"), &identity_key, &found_identity) == 0 &&
		found_identity.local_isn == local_isn && found_identity.remote_isn == remote_isn &&
		found_identity.mask == IDENTITY_MASK_CONFIRMED &&
		bpf_map_lookup_elem(map_fd(harness, "generation_states"), &state_key, state_bytes) == 0 &&
		bpf_map_lookup_elem(map_fd(harness, "departures"), &departure_key, &departure_ns) == 0 &&
		departure_ns != 0U;
	return valid;
}

static uint32_t estimator_generation(
	const struct tcpdelay_estimator *estimator,
	const struct flow *flow
)
{
	size_t index;

	for (index = 0U; index < TCPDELAY_FLOWS; index++) {
		const struct tcpdelay_flow *entry = &estimator->flows[index];

		if (entry->used && memcmp(&entry->key, flow, sizeof(*flow)) == 0)
			return entry->generation;
	}
	return 0U;
}

static uint32_t allocator_value(struct harness *harness)
{
	uint32_t key = 0U;
	uint32_t value = 0U;

	if (bpf_map_lookup_elem(map_fd(harness, "generation_allocator"), &key, &value) != 0)
		fail_errno("read allocator");
	return value;
}

static void set_allocator(struct harness *harness, uint32_t value)
{
	uint32_t key = 0U;

	if (bpf_map_update_elem(map_fd(harness, "generation_allocator"), &key, &value, BPF_ANY) != 0)
		fail_errno("set allocator");
}

static void handshake_active(
	struct harness *harness,
	uint16_t port,
	uint32_t local_isn,
	uint32_t remote_isn,
	uint32_t ts_base
)
{
	struct packet_spec packet = { .local_port = port, .outgoing = 1 };

	packet.flags = TCP_FLAG_SYN;
	packet.sequence = local_isn;
	packet.tsval = ts_base;
	inject(harness, &packet);
	packet.outgoing = 0;
	packet.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	packet.sequence = remote_isn;
	packet.acknowledgment = local_isn + ACK_SEQUENCE_ADVANCE;
	packet.tsval = ts_base;
	packet.tsecr = ts_base;
	inject(harness, &packet);
	packet.outgoing = 1;
	packet.flags = TCP_FLAG_ACK;
	packet.sequence = local_isn + ACK_SEQUENCE_ADVANCE;
	packet.acknowledgment = remote_isn + ACK_SEQUENCE_ADVANCE;
	packet.tsval = ts_base + ACK_SEQUENCE_ADVANCE;
	packet.tsecr = ts_base;
	inject(harness, &packet);
}

static void send_remote_timestamp(
	struct harness *harness,
	uint16_t port,
	uint32_t remote_tsval,
	uint32_t local_tsecr
)
{
	struct packet_spec packet = {
		.local_port = port,
		.sequence = 100U,
		.acknowledgment = 200U,
		.tsval = remote_tsval,
		.tsecr = local_tsecr,
		.flags = TCP_FLAG_ACK,
		.outgoing = 0,
	};

	inject(harness, &packet);
}

static struct counter_values sum_counters(struct harness *harness)
{
	struct counter_values totals = { 0 };
	struct counter_values *per_cpu;
	int cpu_count = libbpf_num_possible_cpus();
	uint32_t key = 0U;
	int cpu;

	if (cpu_count <= 0)
		fail_errno("count CPUs");
	per_cpu = calloc((size_t)cpu_count, sizeof(*per_cpu));
	if (per_cpu == NULL)
		fail_errno("allocate counters");
	if (bpf_map_lookup_elem(map_fd(harness, "counters"), &key, per_cpu) != 0)
		fail_errno("read counters");
	for (cpu = 0; cpu < cpu_count; cpu++) {
		totals.ring_full += per_cpu[cpu].ring_full;
		totals.generation_failures += per_cpu[cpu].generation_failures;
		totals.ack_bytes += per_cpu[cpu].ack_bytes;
		totals.upload_bytes += per_cpu[cpu].upload_bytes;
	}
	free(per_cpu);
	return totals;
}

static void test_handshake_and_direction(struct harness *harness)
{
	const uint16_t port = 50001U;
	struct flow expected = flow_for(port);
	struct counter_values before_counters;
	struct counter_values after_counters;
	size_t before = begin_scenario(harness);
	uint32_t generation;
	struct packet_spec pure_ack = { .local_port = port, .sequence = 1002U,
					.acknowledgment = 2001U, .tsval = 9002U,
					.tsecr = 9001U, .flags = TCP_FLAG_ACK, .outgoing = 1 };
	struct packet_spec incoming_ack = { .local_port = port, .sequence = 2001U,
					     .acknowledgment = 1003U, .tsval = 9002U,
					     .tsecr = 9002U, .flags = TCP_FLAG_ACK, .outgoing = 0 };

	handshake_active(harness, port, 1000U, 2000U, 9000U);
	send_remote_timestamp(harness, port, 9001U, 9001U);
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	generation = lifecycle_generation(harness, &expected);
	check(generation == GENERATION_FIRST, "active_open_generation_started");
	check(records_for_flow(harness, before, &expected) == 2U,
	      "active_open_emitted_handshake_and_data_samples");
	check(handshake_state_valid(harness, &expected, 1000U, 2000U, generation, 9001U),
	      "active_open_lifecycle_identity_pair_pending_state_departure_invariants");
	{
		struct seen_syn_key active_syn = { .flow = expected, .side = SIDE_LOCAL, .isn = 1000U };
		struct seen_syn_key synack_seen = { .flow = expected, .side = SIDE_REMOTE, .isn = 2000U };
		uint8_t value;
		check(bpf_map_lookup_elem(map_fd(harness, "seen_syn"), &active_syn, &value) == 0 &&
		      bpf_map_lookup_elem(map_fd(harness, "seen_syn"), &synack_seen, &value) != 0,
		      "active_open_remembers_pure_syn_only");
	}
	{
		const struct record *record = find_record(harness, before, &expected, generation, 9001U);
		check(record != NULL && record->departure_ns != 0U,
		      "echoed_timestamp_uses_matching_upload_departure");
	}
	before_counters = sum_counters(harness);
	inject(harness, &pure_ack);
	{
		struct counter_values middle = sum_counters(harness);
		uint64_t upload_delta = middle.upload_bytes - before_counters.upload_bytes;
		uint64_t ack_delta = middle.ack_bytes - before_counters.ack_bytes;

		check(upload_delta == PACKET_BUFFER_BYTES, "outgoing_packet_increments_upload_bytes");
		check(ack_delta == PACKET_BUFFER_BYTES, "outgoing_pure_ack_increments_ack_bytes");
		inject(harness, &incoming_ack);
		after_counters = sum_counters(harness);
		check(after_counters.upload_bytes == middle.upload_bytes &&
		      after_counters.ack_bytes == middle.ack_bytes,
		      "incoming_packet_does_not_increment_upload_counters");
	}
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	printf("DATA\tactive_open\tgeneration=%u\trecords=%zu\n",
	       generation,
	       records_for_flow(harness, before, &expected));
}

static void test_passive_open(struct harness *harness)
{
	const uint16_t port = 50002U;
	struct flow flow = flow_for(port);
	size_t before = begin_scenario(harness);
	struct packet_spec packet = { .local_port = port, .sequence = 3000U, .tsval = 300U,
				      .flags = TCP_FLAG_SYN, .outgoing = 0 };

	inject(harness, &packet);
	packet.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	packet.outgoing = 1;
	packet.sequence = 4000U;
	packet.acknowledgment = 3001U;
	packet.tsval++;
	inject(harness, &packet);
	packet.flags = TCP_FLAG_ACK;
	packet.outgoing = 0;
	packet.sequence = 3001U;
	packet.acknowledgment = 4001U;
	packet.tsval++;
	packet.tsecr = 301U;
	inject(harness, &packet);
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	{
		uint32_t generation = lifecycle_generation(harness, &flow);
		const struct record *record = find_record(harness, before, &flow, generation, 302U);

		check(generation != 0U, "passive_open_confirmed");
		check(handshake_state_valid(harness, &flow, 4000U, 3000U, generation, 301U),
		      "passive_open_state_invariants");
		{
			struct seen_syn_key passive_syn = {
				.flow = flow, .side = SIDE_REMOTE, .isn = 3000U,
			};
			struct seen_syn_key synack_seen = {
				.flow = flow, .side = SIDE_LOCAL, .isn = 4000U,
			};
			uint8_t value;
			check(bpf_map_lookup_elem(map_fd(harness, "seen_syn"), &passive_syn, &value) == 0 &&
			      bpf_map_lookup_elem(map_fd(harness, "seen_syn"), &synack_seen, &value) != 0,
			      "passive_open_remembers_peer_pure_syn_only");
		}
		check(records_for_flow(harness, before, &flow) == 1U && record != NULL &&
		      record->departure_ns != 0U,
		      "passive_open_echo_matches_local_synack_departure");
	}
}

static void test_syn_retransmission(struct harness *harness)
{
	const uint16_t port = 50003U;
	struct flow flow = flow_for(port);
	size_t before = begin_scenario(harness);
	uint32_t allocator_before;
	uint32_t generation;
	struct packet_spec packet = { .local_port = port, .sequence = 123U, .tsval = 1U,
				      .flags = TCP_FLAG_SYN, .outgoing = 1 };

	allocator_before = allocator_value(harness);
	inject(harness, &packet);
	inject(harness, &packet);
	packet.outgoing = 0;
	packet.sequence = 456U;
	packet.acknowledgment = 124U;
	packet.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	inject(harness, &packet);
	generation = lifecycle_generation(harness, &flow);
	inject(harness, &packet);
	packet.outgoing = 1;
	packet.sequence = 124U;
	packet.acknowledgment = 457U;
	packet.flags = TCP_FLAG_ACK;
	inject(harness, &packet);
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	check(generation != 0U, "syn_retransmission_handshake_confirmed");
	check(allocator_value(harness) == allocator_before + 1U,
	      "syn_and_synack_retransmissions_do_not_allocate_duplicate_generation");
	check(records_for_flow(harness, before, &flow) == 1U,
	      "retransmitted_synack_does_not_duplicate_sample");
}

static void test_same_tuple_reuse(struct harness *harness)
{
	const uint16_t port = 50004U;
	struct flow flow = flow_for(port);
	size_t before = begin_scenario(harness);
	struct packet_spec syn = { .local_port = port, .sequence = 900U, .tsval = 900U,
				   .flags = TCP_FLAG_SYN, .outgoing = 1 };
	uint32_t first_generation;
	uint32_t second_generation;
	size_t records_before;

	handshake_active(harness, port, 900U, 1900U, 900U);
	send_remote_timestamp(harness, port, 901U, 901U);
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	check(records_for_flow(harness, before, &flow) == 2U,
	      "predecessor_lifetime_emits_authentic_records");
	first_generation = lifecycle_generation(harness, &flow);
	records_before = harness->record_count;
	syn.sequence = 100U;
	syn.tsval = 1U; /* Timestamp clock reset is expected for this new TCP lifetime. */
	inject(harness, &syn);
	syn.outgoing = 0;
	syn.sequence = 200U;
	syn.acknowledgment = 101U;
	syn.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	inject(harness, &syn);
	syn.outgoing = 1;
	syn.sequence = 101U;
	syn.acknowledgment = 201U;
	syn.flags = TCP_FLAG_ACK;
	inject(harness, &syn);
	second_generation = lifecycle_generation(harness, &flow);
	send_remote_timestamp(harness, port, 2U, 901U);
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	check(first_generation != 0U && second_generation > first_generation,
	      "same_tuple_new_isns_advance_generation");
	check(handshake_state_valid(harness, &flow, 100U, 200U, second_generation, 1U),
	      "successor_handshake_state_invariants");
	check(records_for_flow(harness, records_before, &flow) == 2U,
	      "successor_lifetime_emits_authentic_synack_and_data_records");
	{
		const struct record *record = find_record(harness, records_before, &flow,
						  second_generation, 2U);
		check(record != NULL && record->departure_ns == 0U,
		      "old_lifetime_tsecr_cannot_match_successor_departure");
	}
}

static void test_simultaneous_open(struct harness *harness)
{
	const uint16_t port = 50005U;
	struct flow flow = flow_for(port);
	size_t record_start = begin_scenario(harness);
	struct packet_spec packet = { .local_port = port, .sequence = 77U, .tsval = 10U,
				      .flags = TCP_FLAG_SYN, .outgoing = 1 };
	uint32_t expected_generation;

	expected_generation = allocator_value(harness);
	inject(harness, &packet);
	packet.outgoing = 0;
	packet.sequence = 88U;
	packet.tsval++;
	inject(harness, &packet);
	packet.outgoing = 1;
	packet.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	packet.sequence = 77U;
	packet.acknowledgment = 89U;
	packet.tsval++;
	inject(harness, &packet);
	packet.outgoing = 0;
	packet.sequence = 88U;
	packet.acknowledgment = 78U;
	packet.tsval++;
	inject(harness, &packet);
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	check(lifecycle_generation(harness, &flow) == expected_generation,
	      "simultaneous_open_shares_confirmed_generation");
	check(handshake_state_valid(harness, &flow, 77U, 88U, expected_generation, 12U),
	      "simultaneous_open_state_invariants");
	check(records_for_flow(harness, record_start, &flow) == 1U,
	      "simultaneous_open_synack_sample_is_idempotent");
	{
		struct seen_syn_key local_syn = { .flow = flow, .side = SIDE_LOCAL, .isn = 77U };
		struct seen_syn_key remote_syn = { .flow = flow, .side = SIDE_REMOTE, .isn = 88U };
		uint8_t value;
		check(bpf_map_lookup_elem(map_fd(harness, "seen_syn"), &local_syn, &value) == 0 &&
		      bpf_map_lookup_elem(map_fd(harness, "seen_syn"), &remote_syn, &value) == 0,
		      "simultaneous_open_remembers_both_pure_syns");
	}
}

static void test_ack_wrap(struct harness *harness)
{
	const uint16_t port = 50006U;
	struct flow flow = flow_for(port);
	size_t before = begin_scenario(harness);
	struct packet_spec packet = { .local_port = port, .sequence = UINT32_MAX,
				      .tsval = 22U, .flags = TCP_FLAG_SYN, .outgoing = 1 };

	inject(harness, &packet);
	packet.outgoing = 0;
	packet.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	packet.sequence = 100U;
	packet.acknowledgment = 0U; /* UINT32_MAX + 1 wraps in TCP sequence space. */
	inject(harness, &packet);
	packet.outgoing = 1;
	packet.flags = TCP_FLAG_ACK;
	packet.sequence = 0U;
	packet.acknowledgment = 101U;
	inject(harness, &packet);
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	{
		uint32_t generation = lifecycle_generation(harness, &flow);
		check(generation != 0U, "tcp_ack_wrap_confirms_syn");
		check(handshake_state_valid(harness, &flow, UINT32_MAX, 100U, generation, 22U),
		      "ack_wrap_state_invariants");
		check(records_for_flow(harness, before, &flow) == 1U,
		      "ack_wrap_emits_synack_record");
	}
}

static void test_bootstrap(struct harness *harness)
{
	const uint16_t port = 50007U;
	struct flow flow = flow_for(port);
	size_t before = begin_scenario(harness);
	struct packet_spec packet = { .local_port = port, .sequence = 1U, .acknowledgment = 2U,
				      .tsval = 1U, .flags = TCP_FLAG_ACK, .outgoing = 1 };

	check(lifecycle_generation(harness, &flow) == 0U, "bootstrap_fixture_starts_untracked");
	inject(harness, &packet);
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	{
		struct connection_lifecycle lifecycle;
		struct connection_identity identity;
		struct identity_key identity_key;
		struct generation_flow_key state_key;
		struct departure_key departure_key;
		struct side_key local_side = { .flow = flow, .side = SIDE_LOCAL };
		struct side_key remote_side = { .flow = flow, .side = SIDE_REMOTE };
		uint32_t pending;
		uint64_t departure;
		uint32_t generation = lifecycle_generation(harness, &flow);

		identity_key.flow = flow;
		identity_key.generation = generation;
		state_key.flow = flow;
		state_key.generation = generation;
		departure_key.flow = flow;
		departure_key.generation = generation;
		departure_key.tsval = 1U;
		check(generation != 0U, "midstream_flow_bootstraps");
		check(bpf_map_lookup_elem(map_fd(harness, "lifecycles"), &flow, &lifecycle) == 0 &&
		      lifecycle.active_generation == generation && lifecycle.latest_generation == generation,
		      "bootstrap_lifecycle_is_active_and_latest");
		check(bpf_map_lookup_elem(map_fd(harness, "identities"), &identity_key, &identity) == 0 &&
		      identity.mask == 0U && identity.local_isn == 0U && identity.remote_isn == 0U,
		      "bootstrap_identity_is_unconfirmed");
		check(bpf_map_lookup_elem(map_fd(harness, "pending_sides"), &local_side, &pending) != 0 &&
		      bpf_map_lookup_elem(map_fd(harness, "pending_sides"), &remote_side, &pending) != 0,
		      "bootstrap_has_no_pending_syn_sides");
		check(bpf_map_lookup_elem(map_fd(harness, "generation_states"), &state_key,
					  &(uint32_t[8]){ 0 }) == 0 &&
		      bpf_map_lookup_elem(map_fd(harness, "departures"), &departure_key, &departure) == 0,
		      "bootstrap_has_generation_state_and_departure");
		check(records_for_flow(harness, before, &flow) == 0U,
		      "outgoing_bootstrap_packet_does_not_emit_incoming_record");
	}
}

static void test_estimator_late_ring_order(struct harness *harness)
{
	const uint16_t port = 50008U;
	struct flow flow = flow_for(port);
	size_t first = begin_scenario(harness);
	uint32_t old_generation;
	uint32_t new_generation;
	const struct record *old_record;
	const struct record *new_record;
	struct tcpdelay_estimator isolated_estimator;
	struct tcpdelay_sample sample;
	uint64_t accepted_ns;
	struct packet_spec syn = { .local_port = port, .sequence = 111U, .tsval = 50U,
				   .flags = TCP_FLAG_SYN, .outgoing = 1 };

	tcpdelay_estimator_init(&harness->estimator);
	handshake_active(harness, port, 111U, 222U, 50U);
	send_remote_timestamp(harness, port, 51U, 51U);
	old_generation = lifecycle_generation(harness, &flow);
	/* Hold authentic queued old-lifetime data while the successor is activated. */
	syn.sequence = 112U;
	syn.tsval = 1U;
	inject(harness, &syn);
	syn.outgoing = 0;
	syn.sequence = 333U;
	syn.acknowledgment = 113U;
	syn.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	inject(harness, &syn);
	syn.outgoing = 1;
	syn.sequence = 113U;
	syn.acknowledgment = 334U;
	syn.flags = TCP_FLAG_ACK;
	inject(harness, &syn);
	new_generation = lifecycle_generation(harness, &flow);
	{
		struct timespec wait = { .tv_nsec = 5L * 1000000L };
		(void)nanosleep(&wait, NULL);
	}
	send_remote_timestamp(harness, port, 2U, 51U);
	check(consume_ring(harness) == 4, "old_and_new_authentic_records_drain");
	check(records_for_flow(harness, first, &flow) == 4U, "ring_records_are_isolated_by_scenario");
	check(new_generation > old_generation, "ring_successor_generation_is_newer");
	check(estimator_generation(&harness->estimator, &flow) == new_generation,
	      "production_estimator_accepts_successor_in_natural_ring_order");
	old_record = find_record(harness, first, &flow, old_generation, 51U);
	new_record = find_record(harness, first, &flow, new_generation, 2U);
	check(old_record != NULL && old_record->departure_ns != 0U && new_record != NULL &&
	      new_record->departure_ns == 0U,
	      "authentic_old_and_new_records_have_distinct_generation_departures");
	tcpdelay_estimator_init(&isolated_estimator);
	check(new_record != NULL && old_record != NULL, "authentic_records_found_for_reorder_test");
	if (new_record == NULL || old_record == NULL)
		return;
	memcpy(&sample.flow, &new_record->flow, sizeof(sample.flow));
	sample.arrival_ns = new_record->arrival_ns;
	sample.departure_ns = new_record->departure_ns;
	sample.tsval = new_record->tsval;
	sample.generation = new_record->generation;
	tcpdelay_estimator_add(&isolated_estimator, &sample);
	accepted_ns = isolated_estimator.flows[0].last_accepted_ns;
	check(isolated_estimator.flows[0].generation == new_generation,
	      "fresh_estimator_accepts_authentic_successor_first");
	memcpy(&sample.flow, &old_record->flow, sizeof(sample.flow));
	sample.arrival_ns = old_record->arrival_ns;
	sample.departure_ns = old_record->departure_ns;
	sample.tsval = old_record->tsval;
	sample.generation = old_record->generation;
	tcpdelay_estimator_add(&isolated_estimator, &sample);
	check(isolated_estimator.flows[0].generation == new_generation &&
	      isolated_estimator.flows[0].last_accepted_ns == accepted_ns,
	      "delayed_authentic_predecessor_changes_neither_generation_nor_lru_time");
}

static void test_terminal_allocator(struct harness *harness)
{
	const uint16_t port = 52000U;
	struct flow flow = flow_for(port);
	struct flow exhausted_flow = flow_for((uint16_t)(port + 1U));
	struct packet_spec packet = { .local_port = port, .sequence = 99U, .tsval = 1U,
				      .flags = TCP_FLAG_SYN, .outgoing = 1 };
	struct counter_values before;
	struct counter_values after;
	size_t first = begin_scenario(harness);

	before = sum_counters(harness);
	check(before.ring_full == 0U && before.generation_failures == 0U,
	      "pre_terminal_ring_and_generation_health_counters_zero");
	set_allocator(harness, GENERATION_TERMINAL);
	inject(harness, &packet);
	packet.outgoing = 0;
	packet.sequence = 199U;
	packet.acknowledgment = 100U;
	packet.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	inject(harness, &packet);
	packet.outgoing = 1;
	packet.sequence = 100U;
	packet.acknowledgment = 200U;
	packet.flags = TCP_FLAG_ACK;
	inject(harness, &packet);
	check(lifecycle_generation(harness, &flow) == GENERATION_TERMINAL,
	      "terminal_nonzero_generation_is_allocatable");
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	check(records_for_flow(harness, first, &flow) == 1U,
	      "terminal_nonzero_generation_emits_one_authentic_record");
	first = begin_scenario(harness);
	packet.local_port++;
	packet.sequence = 300U;
	packet.flags = TCP_FLAG_SYN;
	inject(harness, &packet);
	packet.outgoing = 0;
	packet.sequence = 400U;
	packet.acknowledgment = 301U;
	packet.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
	inject(harness, &packet);
	packet.outgoing = 1;
	packet.sequence = 301U;
	packet.acknowledgment = 401U;
	packet.flags = TCP_FLAG_ACK;
	inject(harness, &packet);
	check(lifecycle_generation(harness, &exhausted_flow) == 0U,
	      "wrapped_zero_allocator_fails_closed");
	if (consume_ring(harness) < 0)
		exit(EXIT_FAILURE);
	after = sum_counters(harness);
	check(after.generation_failures == before.generation_failures + 1U,
	      "allocator_exhaustion_increments_exactly_one_failure");
	check(after.ring_full == before.ring_full && records_for_flow(harness, first, &exhausted_flow) == 0U,
	      "terminal_failure_emits_no_ring_record");
	{
		struct connection_lifecycle lifecycle;
		struct side_key local_side = { .flow = exhausted_flow, .side = SIDE_LOCAL };
		struct side_key remote_side = { .flow = exhausted_flow, .side = SIDE_REMOTE };
		struct confirmed_pair_key pair = {
			.flow = exhausted_flow, .local_isn = 300U, .remote_isn = 400U,
		};
		struct identity_key identity = { .flow = exhausted_flow, .generation = 0U };
		struct generation_flow_key state = { .flow = exhausted_flow, .generation = 0U };
		struct departure_key departure = { .flow = exhausted_flow, .generation = 0U,
						   .tsval = packet.tsval };
		struct connection_identity identity_value;
		uint32_t pending;
		uint32_t generation_value;
		uint64_t departure_value;
		uint8_t state_value[32];
		check(bpf_map_lookup_elem(map_fd(harness, "lifecycles"), &exhausted_flow,
					  &lifecycle) == 0 && lifecycle.active_generation == 0U &&
		      lifecycle.latest_generation == 0U,
		      "terminal_failure_keeps_lifecycle_inactive");
		check(bpf_map_lookup_elem(map_fd(harness, "pending_sides"), &local_side, &pending) == 0 &&
		      pending == 300U &&
		      bpf_map_lookup_elem(map_fd(harness, "pending_sides"), &remote_side, &pending) == 0 &&
		      pending == 400U,
		      "terminal_failure_retains_only_current_pending_syns");
		check(bpf_map_lookup_elem(map_fd(harness, "confirmed_pairs"), &pair,
					  &generation_value) != 0 &&
		      bpf_map_lookup_elem(map_fd(harness, "identities"), &identity,
					  &identity_value) != 0 &&
		      bpf_map_lookup_elem(map_fd(harness, "generation_states"), &state, state_value) != 0 &&
		      bpf_map_lookup_elem(map_fd(harness, "departures"), &departure,
					  &departure_value) != 0,
		      "terminal_failure_creates_no_pair_identity_state_or_departure");
	}
	printf("DATA\tallocator_terminal\tvalue=%u\tfailures=%llu\n",
	       allocator_value(harness),
	       (unsigned long long)(after.generation_failures - before.generation_failures));
}

static void close_harness(struct harness *harness)
{
	if (harness->transmit_socket >= 0)
		(void)close(harness->transmit_socket);
	if (harness->filter_socket >= 0)
		(void)close(harness->filter_socket);
	ring_buffer__free(harness->ring);
	bpf_object__close(harness->object);
}

int main(int argc, char **argv)
{
	struct harness harness;

	if (argc != 4) {
		fprintf(stderr, "usage: %s OBJECT CAPTURE_VETH PEER_VETH\n", argv[0]);
		return EXIT_FAILURE;
	}
	(void)alarm(120U);
	initialize_harness(&harness, argv[1], argv[2], argv[3]);
	puts("RECORD\tcase\tgeneration\tarrival_ns\tdeparture_ns\ttsval\tflow_local_port");
	test_handshake_and_direction(&harness);
	test_passive_open(&harness);
	test_syn_retransmission(&harness);
	test_same_tuple_reuse(&harness);
	test_simultaneous_open(&harness);
	test_ack_wrap(&harness);
	test_bootstrap(&harness);
	test_estimator_late_ring_order(&harness);
	test_terminal_allocator(&harness);
	puts("UNTESTED\tallocator_cas_contention\tno deterministic retry counter");
	puts("UNTESTED\tindependent_lru_eviction_safety\tcapacity churn not run");
	puts("UNTESTED\truntime_hot_cold_profile\trun_time_ns batches not collected");
	for (size_t index = 0U; index < harness.record_count; index++)
		printf("RECORD\tpacket\t%u\t%llu\t%llu\t%u\t%u\n",
		       harness.records[index].generation,
		       (unsigned long long)harness.records[index].arrival_ns,
		       (unsigned long long)harness.records[index].departure_ns,
		       harness.records[index].tsval,
		       ntohs(harness.records[index].flow.local_port));
	printf("SUMMARY\tassertions=%u\tfailures=%u\trecords=%zu\n",
	       assertions,
	       failures,
	       harness.record_count);
	close_harness(&harness);
	return failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
