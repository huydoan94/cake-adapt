/* SPDX-License-Identifier: MIT */
/* Bounded packet-tap comparison; uses production capture for the new object. */
#define _GNU_SOURCE
#include "tcpdelay/capture.h"
#include "logging/log.h"
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/virtio_net.h>
#include <net/if.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define INTERFACE "ca0"
#define FILTER_NAME "tcpdelay"
#define COUNTERS_NAME "counters"
#define VARIANT_AFTER "after"
#define CASE_RAW "raw"
#define CASE_ETHER "ether"
#define CASE_OVERHEAD "overhead"
#define CASE_MPU "mpu"
#define CASE_NEGATIVE "negative"
#define CASE_ATM "atm"
#define CASE_PTM "ptm"
#define CASE_VLAN "vlan"
#define CASE_QINQ "qinq"
#define CASE_UNKNOWN "unknown"
#define CASE_RAW_UNKNOWN "raw-unknown"
#define CASE_GSO "gso"
#define CASE_RAW_GSO "raw-gso"
#define CASE_DISABLED "disabled"
#define CASE_STALE "stale"
#define PACKET_CAPACITY 2200U

static struct tcpdelay_estimator estimator;

void log_message(enum log_level level, const char *format, ...)
{
	va_list arguments;
	(void)level;
	va_start(arguments, format);
	(void)vfprintf(stderr, format, arguments);
	va_end(arguments);
	(void)fputc('\n', stderr);
}

static void require(int condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "%s: %s\n", operation, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

static void packet_send(
	int socket_fd,
	unsigned int vlan_tags,
	unsigned int unknown,
	unsigned int gso,
	unsigned int data
)
{
	uint8_t buffer[PACKET_CAPACITY] = { 0 };
	unsigned int prefix = gso ? sizeof(struct virtio_net_hdr) : 0U;
	uint8_t *packet = buffer + prefix;
	unsigned int offset = ETH_HLEN + 4U * vlan_tags;
	unsigned int payload = gso ? 2000U : data ? 1448U : 0U;
	unsigned int length = offset + 52U + payload;
	struct ethhdr *ethernet = (struct ethhdr *)packet;
	struct iphdr *ip = (struct iphdr *)(packet + offset);
	struct tcphdr *tcp = (struct tcphdr *)(packet + offset + sizeof(*ip));
	struct sockaddr_ll address = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
		.sll_ifindex = (int)if_nametoindex(INTERFACE),
	};
	unsigned int tag;

	ethernet->h_source[0] = 2U;
	ethernet->h_source[5] = 1U;
	ethernet->h_dest[0] = 2U;
	ethernet->h_dest[5] = 2U;
	ethernet->h_proto = htons((uint16_t)(unknown   ? 0x88b5U :
					     vlan_tags ? ETH_P_8021Q :
							 ETH_P_IP));
	for (tag = 0U; tag < vlan_tags; tag++) {
		uint16_t protocol =
			htons((uint16_t)(tag + 1U < vlan_tags ? ETH_P_8021Q : ETH_P_IP));
		memcpy(packet + ETH_HLEN + tag * 4U + 2U, &protocol, sizeof(protocol));
	}
	ip->version = 4U;
	ip->ihl = 5U;
	ip->ttl = 64U;
	ip->protocol = IPPROTO_TCP;
	ip->tot_len = htons((uint16_t)(52U + payload));
	ip->saddr = htonl(0xc0000201U);
	ip->daddr = htonl(0xc0000202U);
	tcp->source = htons(51000U);
	tcp->dest = htons(443U);
	tcp->doff = 8U;
	tcp->ack = 1U;
	/* No timestamp options: accounting does not require timing records. */
	if (gso) {
		struct virtio_net_hdr *header = (struct virtio_net_hdr *)buffer;
		header->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
		header->hdr_len = (uint16_t)(offset + 52U);
		header->gso_size = 1000U;
	}
	require(sendto(socket_fd,
		       buffer,
		       prefix + length,
		       0,
		       (struct sockaddr *)&address,
		       sizeof(address)) == (ssize_t)(prefix + length),
		"send frame");
}

static struct tcpdelay_counters old_counters(int descriptor)
{
	struct tcpdelay_counters totals = { 0 };
	int cpus = libbpf_num_possible_cpus();
	uint32_t key = 0U;
	uint64_t *values;
	int cpu;

	require(cpus > 0, "possible CPUs");
	values = calloc((size_t)cpus * 3U, sizeof(*values));
	require(values != NULL, "counter buffer");
	require(bpf_map_lookup_elem(descriptor, &key, values) == 0, "old counters");
	for (cpu = 0; cpu < cpus; cpu++) {
		totals.ring_full += values[(size_t)cpu * 3U];
		totals.ack_bytes += values[(size_t)cpu * 3U + 1U];
		totals.upload_bytes += values[(size_t)cpu * 3U + 2U];
	}
	free(values);
	return totals;
}

int main(int argc, char **argv)
{
	struct cake_accounting model = { .atm_mode = CAKE_ATM_NONE };
	struct tcpdelay_capture capture;
	struct bpf_object *object;
	struct bpf_program *program;
	struct bpf_prog_info info = { 0 };
	struct tcpdelay_counters counters;
	struct bpf_map *map;
	const char *name;
	char error[256];
	cpu_set_t affinity;
	uint32_t info_size = sizeof(info);
	unsigned int vlan = 0U, unknown = 0U, gso = 0U, disabled = 0U;
	unsigned long long expected_ack = 52U, expected_upload = 1552U, expected_bad = 0U;
	int after, filter_fd, tx_fd, program_fd, stats_fd, counters_fd;

	require(argc == 4, "usage: probe OBJECT before|after CASE");
	after = strcmp(argv[2], VARIANT_AFTER) == 0;
	name = argv[3];
	CPU_ZERO(&affinity);
	CPU_SET(0, &affinity);
	require(sched_setaffinity(0, sizeof(affinity), &affinity) == 0, "CPU affinity");
	if (strcmp(name, CASE_STALE) == 0) {
		require(tcpdelay_capture_open(
				&capture,
				argv[1],
				INTERFACE,
				&model,
				&estimator,
				error,
				sizeof(error)
			) < 0,
			"reject stale object");
		require(strstr(error, "incompatible accounting maps") != NULL,
			"stale ABI diagnostic");
		puts("PASS stale object safely rejected");
		return 0;
	}
	if (strcmp(name, CASE_RAW) == 0 || strcmp(name, CASE_RAW_UNKNOWN) == 0) {
		model.raw = 1U;
		expected_ack = 66U;
		expected_upload = 1580U;
	} else if (strcmp(name, CASE_OVERHEAD) == 0) {
		model.overhead_bytes = 44;
		expected_ack = 96U;
		expected_upload = 1640U;
	} else if (strcmp(name, CASE_MPU) == 0) {
		model.mpu_bytes = 84U;
		expected_ack = 84U;
		expected_upload = 1584U;
	} else if (strcmp(name, CASE_NEGATIVE) == 0) {
		model.overhead_bytes = -20;
		expected_ack = 32U;
		expected_upload = 1512U;
	} else if (strcmp(name, CASE_ATM) == 0) {
		model.atm_mode = CAKE_ATM_ATM;
		expected_ack = 106U;
		expected_upload = 1802U;
	} else if (strcmp(name, CASE_PTM) == 0) {
		model.atm_mode = CAKE_ATM_PTM;
		expected_ack = 53U;
		expected_upload = 1577U;
	} else if (strcmp(name, CASE_VLAN) == 0)
		vlan = 1U;
	else if (strcmp(name, CASE_QINQ) == 0)
		vlan = 2U;
	else if (strcmp(name, CASE_UNKNOWN) == 0) {
		expected_ack = 0U;
		expected_upload = 0U;
		expected_bad = 2U;
	} else if (strcmp(name, CASE_GSO) == 0 || strcmp(name, CASE_RAW_GSO) == 0) {
		gso = 1U;
		model.raw = strcmp(name, CASE_RAW_GSO) == 0 ? 1U : 0U;
		expected_ack = 0U;
		expected_upload = 0U;
		expected_bad = 1U;
	} else if (strcmp(name, CASE_DISABLED) == 0) {
		disabled = 1U;
		expected_ack = 66U;
		expected_upload = 1580U;
	} else
		require(strcmp(name, CASE_ETHER) == 0, "known case");
	if (vlan) {
		expected_ack = 0U;
		expected_upload = 0U;
		expected_bad = 2U;
	}
	unknown = strcmp(name, CASE_UNKNOWN) == 0 || strcmp(name, CASE_RAW_UNKNOWN) == 0;
	if (unknown)
		expected_ack = 0U;
	stats_fd = bpf_enable_stats(BPF_STATS_RUN_TIME);
	require(stats_fd >= 0, "runtime stats");
	if (after) {
		require(tcpdelay_capture_open(
				&capture,
				argv[1],
				INTERFACE,
				disabled ? NULL : &model,
				&estimator,
				error,
				sizeof(error)
			) == 0,
			error);
		object = capture.object;
		filter_fd = capture.socket_descriptor;
		counters_fd = capture.counters_descriptor;
	} else {
		struct sockaddr_ll address = { .sll_family = AF_PACKET,
					       .sll_protocol = htons(ETH_P_ALL),
					       .sll_ifindex = (int)if_nametoindex(INTERFACE) };
		object = bpf_object__open_file(argv[1], NULL);
		require(object != NULL && bpf_object__load(object) == 0, "baseline load");
		map = bpf_object__find_map_by_name(object, COUNTERS_NAME);
		require(map != NULL && bpf_map__value_size(map) == 24U, "old counter ABI");
		counters_fd = bpf_map__fd(map);
		program_fd = bpf_program__fd(bpf_object__find_program_by_name(object, FILTER_NAME));
		filter_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
		require(filter_fd >= 0 &&
				setsockopt(
					filter_fd,
					SOL_SOCKET,
					SO_ATTACH_BPF,
					&program_fd,
					sizeof(program_fd)
				) == 0 &&
				bind(filter_fd, (struct sockaddr *)&address, sizeof(address)) == 0,
			"baseline attach");
		expected_ack = unknown || gso || vlan ? 0U : 66U;
		expected_upload = gso ? 2066U : 1580U + 8U * vlan;
		expected_bad = 0U;
	}
	program = bpf_object__find_program_by_name(object, FILTER_NAME);
	program_fd = bpf_program__fd(program);
	tx_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	require(tx_fd >= 0, "TX socket");
	if (gso) {
		int enabled = 1;
		require(setsockopt(tx_fd, SOL_PACKET, PACKET_VNET_HDR, &enabled, sizeof(enabled)) ==
				0,
			"vnet header");
	}
	packet_send(tx_fd, vlan, unknown, gso, 0U);
	if (!gso)
		packet_send(tx_fd, vlan, unknown, gso, 1U);
	/* Unshaped isolated qdisc drains synchronously enough within this bound. */
	usleep(100000U);
	if (after)
		require(tcpdelay_capture_counters(&capture, &counters) == 0, "production counters");
	else
		counters = old_counters(counters_fd);
	require(bpf_prog_get_info_by_fd(program_fd, &info, &info_size) == 0, "program metadata");
	printf("case=%s variant=%s ack=%llu upload=%llu unaccounted=%llu ring_full=%llu runs=%llu runtime_ns=%llu jit_bytes=%u xlated_bytes=%u affinity=0\n",
	       name,
	       argv[2],
	       (unsigned long long)counters.ack_bytes,
	       (unsigned long long)counters.upload_bytes,
	       (unsigned long long)counters.unaccounted_packets,
	       (unsigned long long)counters.ring_full,
	       (unsigned long long)info.run_cnt,
	       (unsigned long long)info.run_time_ns,
	       info.jited_prog_len,
	       info.xlated_prog_len);
	require(counters.ack_bytes == expected_ack && counters.upload_bytes == expected_upload &&
			counters.unaccounted_packets == expected_bad && counters.ring_full == 0U,
		"counter expectations");
	require(info.jited_prog_len != 0U && info.run_cnt == (gso ? 1U : 2U),
		"JIT and exact packet count");
	bpf_object__for_each_map(map, object)
	{
		printf("map=%s value_bytes=%u max_entries=%u type=%u\n",
		       bpf_map__name(map),
		       bpf_map__value_size(map),
		       bpf_map__max_entries(map),
		       bpf_map__type(map));
	}
	close(tx_fd);
	if (after)
		tcpdelay_capture_close(&capture);
	else {
		close(filter_fd);
		bpf_object__close(object);
	}
	close(stats_fd);
	puts("PASS");
	return 0;
}
