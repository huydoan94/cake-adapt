#define _GNU_SOURCE

#include "tcpdelay/capture.h"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RUN_TIMEOUT_SECONDS 120U
#define OPTION_FILE_IDENTITY "--file-identity"
#define OPTION_PREFLIGHT "--preflight"
#define RUN_MODE_ACCEPTANCE "acceptance"
#define RUN_MODE_PREFLIGHT "preflight"
#define TCP_OPTIONS_BYTES 12U
#define FRAME_BYTES \
	(sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct tcphdr) + TCP_OPTIONS_BYTES)
#define ASSERTION_NAME_BYTES 128U

struct packet_spec {
	uint16_t port;
	uint32_t sequence;
	uint32_t acknowledgment;
	uint32_t tsval;
	uint32_t tsecr;
	uint8_t flags;
	bool timestamp;
	bool outgoing;
};

static unsigned int assertions;
static unsigned int failures;
static struct tcpdelay_capture *active_capture;
static int active_sender = -1;

static void cleanup(void)
{
	if (active_sender >= 0)
		(void)close(active_sender);
	if (active_capture != NULL)
		tcpdelay_capture_close(active_capture);
}

static void timeout(int signal_number)
{
	(void)signal_number;
	fprintf(stderr, "runner exceeded %u-second bound\n", RUN_TIMEOUT_SECONDS);
	exit(124);
}

static void check(bool passed, const char *name)
{
	assertions++;
	printf("ASSERT\t%s\t%s\n", passed ? "PASS" : "FAIL", name);
	if (!passed)
		failures++;
}

static void die(const char *message)
{
	fprintf(stderr, "%s: %s\n", message, strerror(errno));
	exit(EXIT_FAILURE);
}

static int report_file_identity(const char *path)
{
	struct stat metadata;

	if (stat(path, &metadata) != 0) {
		fprintf(stderr, "stat %s: %s\n", path, strerror(errno));
		return EXIT_FAILURE;
	}
	printf("%" PRIuMAX ":%" PRIuMAX "\n",
	       (uintmax_t)metadata.st_dev,
	       (uintmax_t)metadata.st_ino);
	return EXIT_SUCCESS;
}

static void report_object(struct tcpdelay_capture *capture, const char *path)
{
	struct bpf_program *program = bpf_object__find_program_by_name(capture->object, "tcpdelay");
	struct bpf_prog_info info = { 0 };
	uint32_t info_size = sizeof(info);
	const struct {
		const char *name;
		uint32_t type;
		uint32_t key;
		uint32_t value;
		uint32_t capacity;
	} expected[] = {
		{ "counters", BPF_MAP_TYPE_PERCPU_ARRAY, 4U, 32U, 1U },
		{ "accounting", BPF_MAP_TYPE_ARRAY, 4U, 24U, 1U },
		{ "stream_epoch_v1", BPF_MAP_TYPE_HASH, 4U, 4U, 1U },
		{ "stream_losses", BPF_MAP_TYPE_HASH, 4U, 4U, 64U },
		{ "stream_fault", BPF_MAP_TYPE_PERCPU_ARRAY, 4U, 4U, 1U },
		{ "samples", BPF_MAP_TYPE_RINGBUF, 0U, 0U, TCPDELAY_RING_BYTES },
	};
	size_t index;

	printf("OBJECT\t%s\nABI\tstream_version=%u\trecord_bytes=%zu\tepoch_slots=%u\tloss_slots=%u\n",
	       path,
	       TCPDELAY_STREAM_VERSION,
	       sizeof(struct tcpdelay_record),
	       TCPDELAY_STREAM_EPOCHS,
	       TCPDELAY_STREAM_LOSSES);
	for (index = 0U; index < sizeof(expected) / sizeof(expected[0]); index++) {
		struct bpf_map *map =
			bpf_object__find_map_by_name(capture->object, expected[index].name);
		struct bpf_map_info map_info = { 0 };
		uint32_t map_info_size = sizeof(map_info);
		char assertion[ASSERTION_NAME_BYTES];
		bool valid = map != NULL &&
			     bpf_map_get_info_by_fd(bpf_map__fd(map), &map_info, &map_info_size) ==
				     0;

		if (valid)
			valid = map_info.type == expected[index].type &&
				map_info.key_size == expected[index].key &&
				map_info.value_size == expected[index].value &&
				map_info.max_entries == expected[index].capacity;
		(void)snprintf(assertion, sizeof(assertion), "map_schema_%s", expected[index].name);
		check(valid, assertion);
		if (valid)
			printf("MAP\t%s\ttype=%u\tkey=%u\tvalue=%u\tcapacity=%u\n",
			       expected[index].name,
			       map_info.type,
			       map_info.key_size,
			       map_info.value_size,
			       map_info.max_entries);
	}
	check(program != NULL, "program_tcpdelay_present");
	if (program == NULL)
		return;
	if (bpf_obj_get_info_by_fd(bpf_program__fd(program), &info, &info_size) != 0)
		die("query loaded program info");
	printf("PROGRAM\tid=%u\tjited_prog_len=%u\txlated_prog_len=%u\n",
	       info.id,
	       info.jited_prog_len,
	       info.xlated_prog_len);
	check(info.jited_prog_len != 0U, "program_jit_enabled");
}

static void build_packet(uint8_t *frame, const struct packet_spec *spec)
{
	struct ethhdr *eth = (struct ethhdr *)frame;
	struct iphdr *ip = (struct iphdr *)(frame + sizeof(*eth));
	struct tcphdr *tcp = (struct tcphdr *)(frame + sizeof(*eth) + sizeof(*ip));
	uint8_t *options = (uint8_t *)(tcp + 1);
	const uint8_t local_mac[ETH_ALEN] = { 0x02U, 0U, 0U, 0U, 0U, 1U };
	const uint8_t remote_mac[ETH_ALEN] = { 0x02U, 0U, 0U, 0U, 0U, 2U };
	struct in_addr local;
	struct in_addr remote;

	memset(frame, 0, FRAME_BYTES);
	if (inet_pton(AF_INET, "192.0.2.1", &local) != 1 ||
	    inet_pton(AF_INET, "192.0.2.2", &remote) != 1)
		abort();
	memcpy(eth->h_source, spec->outgoing ? local_mac : remote_mac, ETH_ALEN);
	memcpy(eth->h_dest, spec->outgoing ? remote_mac : local_mac, ETH_ALEN);
	eth->h_proto = htons(ETH_P_IP);
	ip->version = 4U;
	ip->ihl = 5U;
	ip->ttl = 64U;
	ip->protocol = IPPROTO_TCP;
	ip->tot_len = htons((uint16_t)(sizeof(*ip) + sizeof(*tcp) +
				       (spec->timestamp ? TCP_OPTIONS_BYTES : 0U)));
	ip->saddr = spec->outgoing ? local.s_addr : remote.s_addr;
	ip->daddr = spec->outgoing ? remote.s_addr : local.s_addr;
	tcp->source = htons(spec->outgoing ? spec->port : 443U);
	tcp->dest = htons(spec->outgoing ? 443U : spec->port);
	tcp->seq = htonl(spec->sequence);
	tcp->ack_seq = htonl(spec->acknowledgment);
	tcp->doff = (uint8_t)((sizeof(*tcp) + (spec->timestamp ? TCP_OPTIONS_BYTES : 0U)) /
			      sizeof(uint32_t));
	tcp->syn = (spec->flags & TCPDELAY_TCP_SYN) != 0U;
	tcp->ack = (spec->flags & TCPDELAY_TCP_ACK) != 0U;
	tcp->fin = (spec->flags & TCPDELAY_TCP_FIN) != 0U;
	tcp->rst = (spec->flags & TCPDELAY_TCP_RST) != 0U;
	if (spec->timestamp) {
		options[0] = 1U;
		options[1] = 1U;
		options[2] = 8U;
		options[3] = 10U;
		uint32_t value = htonl(spec->tsval);
		uint32_t echo = htonl(spec->tsecr);

		memcpy(options + 4U, &value, sizeof(value));
		memcpy(options + 8U, &echo, sizeof(echo));
	}
}

static void send_packet(int descriptor, unsigned int ifindex, const struct packet_spec *spec)
{
	uint8_t frame[FRAME_BYTES];
	struct sockaddr_ll address = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_IP),
		.sll_ifindex = (int)ifindex,
		.sll_halen = ETH_ALEN,
	};

	build_packet(frame, spec);
	if (sendto(descriptor,
		   frame,
		   sizeof(frame),
		   0,
		   (struct sockaddr *)&address,
		   sizeof(address)) != (ssize_t)sizeof(frame))
		die("send synthetic TCP frame");
}

static bool drain_until(struct tcpdelay_capture *capture, unsigned int wanted)
{
	unsigned int attempt;

	for (attempt = 0U; attempt < 100U; attempt++) {
		int result = tcpdelay_capture_drain(capture);

		if (result < 0)
			return false;
		if ((unsigned int)result >= wanted)
			return true;
		struct timespec pause = { .tv_nsec = 10000000L };
		(void)nanosleep(&pause, NULL);
	}
	return false;
}

static struct tcpdelay_lifetime_slot *
find_flow(struct tcpdelay_capture *capture, const struct tcpdelay_record_flow *flow)
{
	size_t index;

	for (index = 0U; index < TCPDELAY_LIFETIME_SLOTS; index++) {
		struct tcpdelay_lifetime_slot *slot = &capture->lifetime->flows[index];

		if (slot->used && memcmp(&slot->flow, flow, sizeof(*flow)) == 0)
			return slot;
	}
	return NULL;
}

static int run(const char *object_path, const char *capture_if, const char *peer_if, bool preflight)
{
	struct tcpdelay_capture capture;
	struct tcpdelay_estimator estimator;
	struct tcpdelay_record_flow flow = { 0 };
	char error[256];
	int sender;
	unsigned int capture_index = if_nametoindex(capture_if);
	unsigned int peer_index = if_nametoindex(peer_if);
	struct packet_spec packet = {
		.port = 51001U,
		.sequence = UINT32_MAX,
		.flags = TCPDELAY_TCP_SYN,
		.outgoing = true,
	};
	struct tcpdelay_lifetime_slot *slot;

	if (capture_index == 0U || peer_index == 0U) {
		errno = ENODEV;
		die("resolve supplied veth interfaces");
	}
	tcpdelay_estimator_init(&estimator);
	if (tcpdelay_capture_open(
		    &capture,
		    object_path,
		    capture_if,
		    NULL,
		    &estimator,
		    error,
		    sizeof(error)
	    ) != 0) {
		fprintf(stderr, "capture open: %s\n", error);
		return EXIT_FAILURE;
	}
	active_capture = &capture;
	report_object(&capture, object_path);
	sender = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (sender < 0)
		die("open raw sender socket");
	active_sender = sender;
	/* Production keys use IPv4-mapped IPv6 addresses. */
	flow.local_address[10] = 0xffU;
	flow.local_address[11] = 0xffU;
	if (inet_pton(AF_INET, "192.0.2.1", flow.local_address + 12U) != 1 ||
	    inet_pton(AF_INET, "192.0.2.2", flow.remote_address + 12U) != 1)
		abort();
	flow.remote_address[10] = 0xffU;
	flow.remote_address[11] = 0xffU;
	flow.local_port = htons(packet.port);
	flow.remote_port = htons(443U);
	send_packet(sender, capture_index, &packet);
	check(drain_until(&capture, 1U), "non_timestamp_syn_consumed");
	slot = find_flow(&capture, &flow);
	check(slot != NULL && slot->pending[0] && slot->pending_isn[0] == packet.sequence,
	      "lifecycle_syn_without_timestamps_tracks_native_identity");
	if (!preflight) {
		packet.timestamp = true;
		packet.tsval = 10U;
		send_packet(sender, capture_index, &packet);
		bool retransmit_drained = drain_until(&capture, 1U);
		slot = find_flow(&capture, &flow);
		check(retransmit_drained && slot != NULL && slot->pending[0] &&
			      slot->pending_isn[0] == packet.sequence && slot->generation == 0U,
		      "active_open_syn_retransmit_keeps_pending_generation");
		packet.flags = TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK;
		packet.sequence = 0x12345678U;
		packet.acknowledgment = 0U;
		packet.timestamp = true;
		packet.tsval = UINT32_MAX - 2U;
		packet.tsecr = 100U;
		packet.outgoing = false;
		send_packet(sender, peer_index, &packet);
		bool synack_drained = drain_until(&capture, 1U);
		packet.flags = TCPDELAY_TCP_ACK;
		packet.sequence = 0U;
		packet.acknowledgment = 0x12345679U;
		packet.tsval = 101U;
		packet.tsecr = UINT32_MAX - 2U;
		packet.outgoing = true;
		send_packet(sender, capture_index, &packet);
		bool ack_drained = drain_until(&capture, 1U);
		slot = find_flow(&capture, &flow);
		check(synack_drained && ack_drained && slot != NULL && slot->active &&
			      slot->confirmed && slot->generation != 0U &&
			      slot->local_isn == UINT32_MAX && slot->remote_isn == 0x12345678U,
		      "active_handshake_confirms_native_generation_across_sequence_wrap");
		if (slot != NULL) {
			uint64_t original_generation = slot->generation;

			packet.flags = TCPDELAY_TCP_SYN;
			packet.sequence = UINT32_MAX;
			packet.tsval = 200U;
			packet.outgoing = true;
			send_packet(sender, capture_index, &packet);
			bool same_isn_drained = drain_until(&capture, 1U);
			packet.flags = TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK;
			packet.sequence = 0x87654321U;
			packet.acknowledgment = 0U;
			packet.tsval = 201U;
			packet.outgoing = false;
			send_packet(sender, peer_index, &packet);
			bool new_peer_drained = drain_until(&capture, 1U);
			slot = find_flow(&capture, &flow);
			check(same_isn_drained && new_peer_drained && slot != NULL &&
				      slot->confirmed && slot->generation != original_generation &&
				      slot->local_isn == UINT32_MAX &&
				      slot->remote_isn == 0x87654321U,
			      "same_local_isn_with_new_peer_is_a_new_generation");
		}
		{
			struct packet_spec passive = {
				.port = 51002U,
				.sequence = 0x0fffffffU,
				.flags = TCPDELAY_TCP_SYN,
				.outgoing = false,
			};
			struct tcpdelay_record_flow passive_flow = flow;

			passive_flow.local_port = htons(passive.port);
			passive.outgoing = false;
			send_packet(sender, peer_index, &passive);
			bool passive_syn_drained = drain_until(&capture, 1U);
			passive.flags = TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK;
			passive.sequence = 0x76543210U;
			passive.acknowledgment = 0x10000000U;
			passive.outgoing = true;
			send_packet(sender, capture_index, &passive);
			bool passive_synack_drained = drain_until(&capture, 1U);
			slot = find_flow(&capture, &passive_flow);
			check(passive_syn_drained && passive_synack_drained && slot != NULL &&
				      slot->active && slot->confirmed &&
				      slot->local_isn == 0x76543210U &&
				      slot->remote_isn == 0x0fffffffU,
			      "passive_open_pairs_remote_syn_with_local_synack");
		}
		check(tcpdelay_capture_timing_available(&capture),
		      "bounded_capture_drain_leaves_timing_available");
	}
	cleanup();
	active_sender = -1;
	active_capture = NULL;
	printf("RESULT\tassertions=%u\tfailures=%u\tmode=%s\n",
	       assertions,
	       failures,
	       preflight ? RUN_MODE_PREFLIGHT : RUN_MODE_ACCEPTANCE);
	return failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	bool preflight = false;
	int first = 1;

	if (argc == 3 && strcmp(argv[1], OPTION_FILE_IDENTITY) == 0)
		return report_file_identity(argv[2]);
	(void)signal(SIGALRM, timeout);
	(void)atexit(cleanup);
	(void)alarm(RUN_TIMEOUT_SECONDS);
	if (argc > 1 && strcmp(argv[1], OPTION_PREFLIGHT) == 0) {
		preflight = true;
		first++;
	}
	if (argc - first != 3) {
		fprintf(stderr,
			"usage: %s [%s] OBJECT CAPTURE_IF PEER_IF\n",
			argv[0],
			OPTION_PREFLIGHT);
		return EXIT_FAILURE;
	}
	return run(argv[first], argv[first + 1], argv[first + 2], preflight);
}
