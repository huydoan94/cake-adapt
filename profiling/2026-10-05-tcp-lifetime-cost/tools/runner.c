/* SPDX-License-Identifier: MIT */
/* Bounded before/after socket-filter runtime probe. */
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
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ETH_BYTES sizeof(struct ethhdr)
#define IP_BYTES sizeof(struct iphdr)
#define TCP_BYTES (sizeof(struct tcphdr) + 12U)
#define PACKET_BYTES (ETH_BYTES + IP_BYTES + TCP_BYTES)
#define STEADY_PAIRS 400U
#define CHURN_FLOWS 64U
#define RING_DRAIN_INTERVAL 64U
#define ACK_ADVANCE 1U
#define CPU_INDEX 0U
#define LOCAL_ADDRESS "192.0.2.1"
#define REMOTE_ADDRESS "192.0.2.2"
#define SERVER_PORT 443U
#define STEADY_PORT 51000U
#define CHURN_PORT_BASE 52000U
#define TCP_FLAG_ACK 0x10U
#define TCP_FLAG_SYN 0x02U
#define TCP_FLAG_SYN_ACK (TCP_FLAG_SYN | TCP_FLAG_ACK)
#define MAP_ALLOCATOR "generation_allocator"
#define MAP_COUNTERS "counters"
#define MAP_SAMPLES "samples"
#define PROGRAM_FILTER "tcpdelay"
#define WORKLOAD_STEADY_NAME "steady"
#define WORKLOAD_CHURN_NAME "churn"

enum workload_kind {
	WORKLOAD_STEADY,
	WORKLOAD_CHURN,
};

struct packet_spec {
	uint16_t port;
	uint32_t sequence;
	uint32_t acknowledgment;
	uint32_t tsval;
	uint32_t tsecr;
	uint8_t flags;
	int outgoing;
};

struct probe {
	struct bpf_object *object;
	struct ring_buffer *ring;
	int program_fd;
	int tx_fd;
	int filter_fd;
	int stats_fd;
	int ifindex;
	int peer_ifindex;
	uint64_t ring_records;
};

static void fail(const char *operation)
{
	fprintf(stderr, "%s: %s\n", operation, strerror(errno));
	exit(EXIT_FAILURE);
}

static int ring_record(void *context, void *data, size_t size)
{
	struct probe *probe = context;

	(void)data;
	if (size == 0U)
		return -1;
	probe->ring_records++;
	return 0;
}

static void packet_build(uint8_t packet[PACKET_BYTES], const struct packet_spec *spec)
{
	struct ethhdr *eth = (struct ethhdr *)packet;
	struct iphdr *ip = (struct iphdr *)(packet + ETH_BYTES);
	struct tcphdr *tcp = (struct tcphdr *)(packet + ETH_BYTES + IP_BYTES);
	uint8_t *options = packet + ETH_BYTES + IP_BYTES + sizeof(*tcp);
	const uint8_t local_mac[ETH_ALEN] = { 2U, 0U, 0U, 0U, 0U, 1U };
	const uint8_t remote_mac[ETH_ALEN] = { 2U, 0U, 0U, 0U, 0U, 2U };
	struct in_addr local;
	struct in_addr remote;

	memset(packet, 0, PACKET_BYTES);
	if (inet_pton(AF_INET, LOCAL_ADDRESS, &local) != 1 ||
	    inet_pton(AF_INET, REMOTE_ADDRESS, &remote) != 1)
		abort();
	memcpy(eth->h_source, spec->outgoing ? local_mac : remote_mac, ETH_ALEN);
	memcpy(eth->h_dest, spec->outgoing ? remote_mac : local_mac, ETH_ALEN);
	eth->h_proto = htons(ETH_P_IP);
	ip->version = 4U;
	ip->ihl = 5U;
	ip->ttl = 64U;
	ip->protocol = IPPROTO_TCP;
	ip->tot_len = htons((uint16_t)(IP_BYTES + TCP_BYTES));
	ip->saddr = spec->outgoing ? local.s_addr : remote.s_addr;
	ip->daddr = spec->outgoing ? remote.s_addr : local.s_addr;
	tcp->source = htons(spec->outgoing ? spec->port : SERVER_PORT);
	tcp->dest = htons(spec->outgoing ? SERVER_PORT : spec->port);
	tcp->seq = htonl(spec->sequence);
	tcp->ack_seq = htonl(spec->acknowledgment);
	tcp->doff = (uint8_t)(TCP_BYTES / 4U);
	tcp->ack = (spec->flags & TCP_FLAG_ACK) != 0U;
	tcp->syn = (spec->flags & TCP_FLAG_SYN) != 0U;
	options[0] = 1U;
	options[1] = 1U;
	options[2] = 8U;
	options[3] = 10U;
	memcpy(options + 4U, &(uint32_t){ htonl(spec->tsval) }, sizeof(uint32_t));
	memcpy(options + 8U, &(uint32_t){ htonl(spec->tsecr) }, sizeof(uint32_t));
}

static void packet_send(struct probe *probe, const struct packet_spec *spec)
{
	uint8_t packet[PACKET_BYTES];
	struct sockaddr_ll address = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_IP),
		.sll_ifindex = spec->outgoing ? probe->ifindex : probe->peer_ifindex,
	};
	ssize_t sent;

	packet_build(packet, spec);
	sent =
		sendto(probe->tx_fd,
		       packet,
		       sizeof(packet),
		       0,
		       (struct sockaddr *)&address,
		       sizeof(address));
	if (sent != (ssize_t)sizeof(packet))
		fail("send timestamp packet");
}

static void ring_drain(struct probe *probe)
{
	int result = ring_buffer__consume(probe->ring);

	if (result < 0) {
		errno = -result;
		fail("drain ring buffer");
	}
}

static void packet_counted(struct probe *probe, const struct packet_spec *spec, unsigned int *count)
{
	packet_send(probe, spec);
	(*count)++;
	if ((*count % RING_DRAIN_INTERVAL) == 0U)
		ring_drain(probe);
}

static void handshake(
	struct probe *probe,
	uint16_t port,
	uint32_t local_isn,
	uint32_t remote_isn,
	uint32_t tsval,
	unsigned int *count
)
{
	struct packet_spec packet = {
		.port = port,
		.sequence = local_isn,
		.tsval = tsval,
		.flags = TCP_FLAG_SYN,
		.outgoing = 1,
	};

	packet_counted(probe, &packet, count);
	packet.outgoing = 0;
	packet.sequence = remote_isn;
	packet.acknowledgment = local_isn + ACK_ADVANCE;
	packet.tsval = tsval;
	packet.tsecr = tsval;
	packet.flags = TCP_FLAG_SYN_ACK;
	packet_counted(probe, &packet, count);
	packet.outgoing = 1;
	packet.sequence = local_isn + ACK_ADVANCE;
	packet.acknowledgment = remote_isn + ACK_ADVANCE;
	packet.tsval = tsval + ACK_ADVANCE;
	packet.flags = TCP_FLAG_ACK;
	packet_counted(probe, &packet, count);
}

static void sample_stats(struct probe *probe, struct bpf_prog_info *info)
{
	__u32 info_size = sizeof(*info);

	memset(info, 0, sizeof(*info));
	if (bpf_prog_get_info_by_fd(probe->program_fd, info, &info_size) != 0)
		fail("read BPF program runtime stats");
}

static void report_maps(struct probe *probe)
{
	struct bpf_map *map;
	int cpu_count = libbpf_num_possible_cpus();

	bpf_object__for_each_map(map, probe->object)
	{
		struct bpf_map_info info = { 0 };
		__u32 info_size = sizeof(info);
		int descriptor = bpf_map__fd(map);
		uint64_t payload_lower_bound = 0U;
		uint64_t ring_reservation = 0U;
		char path[64];
		char line[256];
		FILE *fdinfo;

		if (bpf_map_get_info_by_fd(descriptor, &info, &info_size) != 0)
			fail("read map info");
		if (info.type == BPF_MAP_TYPE_LRU_HASH || info.type == BPF_MAP_TYPE_HASH)
			payload_lower_bound = (uint64_t)info.max_entries *
					      ((uint64_t)info.key_size + info.value_size);
		else if (info.type == BPF_MAP_TYPE_PERCPU_ARRAY && cpu_count > 0)
			payload_lower_bound =
				(uint64_t)info.max_entries * info.value_size * (uint64_t)cpu_count;
		else if (info.type == BPF_MAP_TYPE_ARRAY)
			payload_lower_bound = (uint64_t)info.max_entries * info.value_size;
		else if (info.type == BPF_MAP_TYPE_RINGBUF)
			ring_reservation = info.max_entries;
		printf("MAP\t%.*s\ttype=%u\tkey=%u\tvalue=%u\tmax=%u\tflags=%u\tmax_payload_lower_bound_bytes=%llu\tring_reserved_bytes=%llu\n",
		       BPF_OBJ_NAME_LEN,
		       info.name,
		       info.type,
		       info.key_size,
		       info.value_size,
		       info.max_entries,
		       info.map_flags,
		       (unsigned long long)payload_lower_bound,
		       (unsigned long long)ring_reservation);
		(void)snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", descriptor);
		fdinfo = fopen(path, "r");
		if (fdinfo != NULL) {
			while (fgets(line, sizeof(line), fdinfo) != NULL)
				printf("FDINFO\t%.*s\t%s", BPF_OBJ_NAME_LEN, info.name, line);
			(void)fclose(fdinfo);
		}
	}
}

static void report_program(struct probe *probe)
{
	struct bpf_prog_info info = { 0 };
	__u32 info_size = sizeof(info);
	char path[64];
	char line[256];
	FILE *fdinfo;

	if (bpf_prog_get_info_by_fd(probe->program_fd, &info, &info_size) != 0)
		fail("read program info");
	printf("PROGRAM\t%s\tprog_type=%u\txlated_bytes=%u\tjited_bytes=%u\n",
	       PROGRAM_FILTER,
	       info.type,
	       info.xlated_prog_len,
	       info.jited_prog_len);
	(void)snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", probe->program_fd);
	fdinfo = fopen(path, "r");
	if (fdinfo == NULL)
		return;
	while (fgets(line, sizeof(line), fdinfo) != NULL)
		printf("PROGRAM_FDINFO\t%s\t%s", PROGRAM_FILTER, line);
	(void)fclose(fdinfo);
}

static uint64_t ring_full_count(struct probe *probe)
{
	struct bpf_map_info info = { 0 };
	struct bpf_map *map = bpf_object__find_map_by_name(probe->object, MAP_COUNTERS);
	__u32 info_size = sizeof(info);
	uint32_t key = 0U;
	int cpu_count;
	void *values;
	uint64_t total = 0U;
	int cpu;

	if (map == NULL)
		return 0U;
	if (bpf_map_get_info_by_fd(bpf_map__fd(map), &info, &info_size) != 0)
		fail("read counter map info");
	if (info.value_size < sizeof(total))
		fail("counter map value too small");
	cpu_count = libbpf_num_possible_cpus();
	if (cpu_count <= 0)
		fail("count possible CPUs");
	values = calloc((size_t)cpu_count, info.value_size);
	if (values == NULL)
		fail("allocate per-CPU counter buffer");
	if (bpf_map_lookup_elem(bpf_map__fd(map), &key, values) != 0)
		fail("read ring-full counters");
	for (cpu = 0; cpu < cpu_count; cpu++) {
		const uint8_t *value = (const uint8_t *)values + (size_t)cpu * info.value_size;
		uint64_t cpu_ring_full;

		memcpy(&cpu_ring_full, value, sizeof(cpu_ring_full));
		total += cpu_ring_full;
	}
	free(values);
	return total;
}

static void run_workload(struct probe *probe, enum workload_kind kind)
{
	struct bpf_prog_info before;
	struct bpf_prog_info after;
	uint64_t ring_full_before = ring_full_count(probe);
	unsigned int count = 0U;
	unsigned int index;
	uint32_t tsval = 1000U;

	probe->ring_records = 0U;
	sample_stats(probe, &before);
	if (kind == WORKLOAD_STEADY) {
		const uint16_t port = STEADY_PORT;
		uint32_t sequence = 1001U;
		uint32_t acknowledgment = 2001U;

		handshake(probe, port, 1000U, 2000U, tsval, &count);
		for (index = 0U; index < STEADY_PAIRS; index++) {
			struct packet_spec packet;

			tsval++;
			packet = (struct packet_spec){
				.port = port,
				.sequence = sequence,
				.acknowledgment = acknowledgment,
				.tsval = tsval,
				.tsecr = tsval - 1U,
				.flags = TCP_FLAG_ACK,
				.outgoing = 1,
			};
			packet_counted(probe, &packet, &count);
			packet.outgoing = 0;
			packet.sequence = acknowledgment;
			packet.acknowledgment = sequence + ACK_ADVANCE;
			tsval++;
			packet.tsval = tsval;
			packet.tsecr = tsval - 1U;
			packet_counted(probe, &packet, &count);
			sequence++;
			acknowledgment++;
		}
	} else {
		for (index = 0U; index < CHURN_FLOWS; index++) {
			handshake(
				probe,
				(uint16_t)(CHURN_PORT_BASE + index),
				3000U + index * 3U,
				4000U + index * 3U,
				tsval + index * 4U,
				&count
			);
		}
	}
	{
		const struct timespec settle = { .tv_nsec = 20000000L };
		(void)nanosleep(&settle, NULL);
	}
	ring_drain(probe);
	sample_stats(probe, &after);
	{
		uint64_t run_count = after.run_cnt - before.run_cnt;
		uint64_t run_time = after.run_time_ns - before.run_time_ns;
		uint64_t ring_full_delta = ring_full_count(probe) - ring_full_before;
		const char *name = kind == WORKLOAD_STEADY ? WORKLOAD_STEADY_NAME :
							     WORKLOAD_CHURN_NAME;
		int accepted = run_count == count && ring_full_delta == 0U;

		printf("COST\t%s\tpackets=%u\tbpf_runs=%llu\tbpf_ns=%llu\tns_per_run=%.3f\tring_records=%llu\tring_full=%llu\tcoverage=%s\n",
		       name,
		       count,
		       (unsigned long long)run_count,
		       (unsigned long long)run_time,
		       run_count == 0U ? 0.0 : (double)run_time / (double)run_count,
		       (unsigned long long)probe->ring_records,
		       (unsigned long long)ring_full_delta,
		       accepted ? "PASS" : "FAIL");
		if (!accepted)
			exit(EXIT_FAILURE);
	}
}

static void
probe_open(struct probe *probe, const char *object_path, const char *interface, const char *peer)
{
	struct bpf_program *program;
	struct sockaddr_ll address = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL) };
	struct bpf_map *allocator;
	uint32_t zero = 0U;
	uint32_t initial_generation = 1U;
	cpu_set_t cpu_set;

	memset(probe, 0, sizeof(*probe));
	probe->ifindex = (int)if_nametoindex(interface);
	probe->peer_ifindex = (int)if_nametoindex(peer);
	if (probe->ifindex == 0 || probe->peer_ifindex == 0)
		fail("resolve veth");
	CPU_ZERO(&cpu_set);
	CPU_SET(CPU_INDEX, &cpu_set);
	if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0)
		fail("pin workload to CPU 0");
	probe->stats_fd = bpf_enable_stats(BPF_STATS_RUN_TIME);
	if (probe->stats_fd < 0)
		fail("enable scoped BPF runtime stats");
	probe->object = bpf_object__open_file(object_path, NULL);
	if (probe->object == NULL)
		fail("open BPF object");
	if (bpf_object__load(probe->object) != 0)
		fail("load BPF object");
	allocator = bpf_object__find_map_by_name(probe->object, MAP_ALLOCATOR);
	if (allocator != NULL &&
	    bpf_map_update_elem(bpf_map__fd(allocator), &zero, &initial_generation, BPF_ANY) != 0)
		fail("initialize generation allocator");
	program = bpf_object__find_program_by_name(probe->object, PROGRAM_FILTER);
	if (program == NULL)
		fail("find filter program");
	probe->program_fd = bpf_program__fd(program);
	probe->ring = ring_buffer__new(
		bpf_object__find_map_fd_by_name(probe->object, MAP_SAMPLES),
		ring_record,
		probe,
		NULL
	);
	if (probe->ring == NULL)
		fail("create ring consumer");
	probe->filter_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (probe->filter_fd < 0)
		fail("open filter socket");
	if (setsockopt(
		    probe->filter_fd,
		    SOL_SOCKET,
		    SO_ATTACH_BPF,
		    &probe->program_fd,
		    sizeof(probe->program_fd)
	    ) != 0)
		fail("attach filter");
	address.sll_ifindex = probe->ifindex;
	if (bind(probe->filter_fd, (struct sockaddr *)&address, sizeof(address)) != 0)
		fail("bind filter socket");
	probe->tx_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (probe->tx_fd < 0)
		fail("open transmit socket");
}

int main(int argc, char **argv)
{
	struct probe probe;

	if (argc != 4) {
		fprintf(stderr, "usage: %s OBJECT CAPTURE_VETH PEER_VETH\n", argv[0]);
		return EXIT_FAILURE;
	}
	(void)alarm(120U);
	probe_open(&probe, argv[1], argv[2], argv[3]);
	report_maps(&probe);
	report_program(&probe);
	run_workload(&probe, WORKLOAD_STEADY);
	run_workload(&probe, WORKLOAD_CHURN);
	ring_buffer__free(probe.ring);
	bpf_object__close(probe.object);
	close(probe.tx_fd);
	close(probe.filter_fd);
	close(probe.stats_fd);
	return EXIT_SUCCESS;
}
