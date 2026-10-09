// SPDX-License-Identifier: GPL-2.0-only
/*
 * Socket filter for an AF_PACKET socket on the upload (WAN) interface. Packet
 * taps see outgoing packets after the root qdisc and incoming packets before
 * the ingress redirect to the IFB, so the delays measured from these times
 * exclude our own CAKE queues and include the ISP's.
 *
 * Outgoing: count all bytes and pure-ACK bytes, and record when a new TSval of
 * a flow first left, at most once per TCPDELAY_SAMPLE_INTERVAL_NS.
 * Incoming: emit the remote TSval and the departure of the TSval it echoes
 * (TSecr) when a new TSecr echoes a recorded departure, and otherwise at most
 * once per interval on a change of either. The first packet after a change
 * carries the minimum delay of its group, which is all the estimator keeps.
 *
 * The filter always returns 0, so no packet is copied to the socket.
 *
 * Experimental TCP timestamp injection (tcp_ts_request; inject.h has
 * the policy) lives here too, so both share their maps and incoming packets
 * are read once: the tcx egress program at the end appends a timestamp to
 * IPv6 SYNs that lack one, before the root qdisc, and this filter settles each
 * injected handshake from the server's SYN-ACK or reset. Appending that
 * option is the only change made to any packet.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/if_packet.h>
#include <linux/tcp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#include "tcpdelay/inject.h"

#define TCP_OPTIONS_MAX 40U
#define TCP_OPTION_END 0U
#define TCP_OPTION_NOP 1U
#define TCP_OPTION_TIMESTAMP 8U
#define TCP_OPTION_TIMESTAMP_LENGTH 10U
#define TCP_HEADER_BYTES 20U
#define TCP_HEADER_MAX_BYTES 60U
#define TCP_FLAGS_OFFSET 12U
#define TCP_CHECKSUM_OFFSET 16U
#define IPV6_LENGTH_OFFSET 4U
/* NOP, NOP and a timestamp option, appended to a SYN. */
#define INJECT_ADDED_BYTES 12U

struct departure_key {
	struct tcpdelay_record_flow flow;
	__u32 tsval;
};

/* Racy updates from several CPUs only cost an extra or a skipped sample. */
struct flow_state {
	__u64 departure_ns;
	__u64 sample_ns;
	__u32 outgoing_tsval;
	__u32 incoming_tsval;
	__u32 incoming_tsecr;
	/*
	 * A timestamp left on a packet other than the SYN: the client's own clock,
	 * where an injected SYN carries ours (the tap sees packets after the
	 * injector).
	 */
	__u32 client_stamped;
	/*
	 * An injected handshake whose SYN-ACK echoed our timestamp, waiting for
	 * the server to acknowledge the client's first data; its SYN's sequence.
	 */
	__u32 inject_waiting;
	__u32 inject_sequence;
};

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, TCPDELAY_DEPARTURES);
	__type(key, struct departure_key);
	__type(value, __u64);
} departures SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, TCPDELAY_FLOW_STATES);
	__type(key, struct tcpdelay_record_flow);
	__type(value, struct flow_state);
} flows SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, TCPDELAY_RING_BYTES);
} samples SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct tcpdelay_counters);
} counters SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct tcpdelay_accounting);
} accounting SEC(".maps");

/* Injection; sized down to a few entries when it is off. */
struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, TCPDELAY_INJECT_SERVERS);
	__type(key, struct tcpdelay_inject_server_key);
	__type(value, struct tcpdelay_inject_server);
} inject_servers SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, TCPDELAY_INJECT_HANDSHAKES);
	__type(key, struct tcpdelay_record_flow);
	__type(value, struct tcpdelay_inject_handshake);
} inject_handshakes SEC(".maps");

/* Clients' learned clocks, by client address. */
struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, TCPDELAY_INJECT_CLIENTS);
	__type(key, struct tcpdelay_inject_client_key);
	__type(value, struct tcpdelay_inject_client);
} inject_clients SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct tcpdelay_inject_counters);
} inject_counters SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct tcpdelay_inject_settings);
} inject_settings SEC(".maps");

static __always_inline int ip_protocol(__be16 protocol)
{
	return protocol == bpf_htons(ETH_P_IP) || protocol == bpf_htons(ETH_P_IPV6);
}

/*
 * Both counters use this one charge. Unsupported traffic leaves timing
 * available. Packet taps push the Ethernet header back before running a RAW
 * socket filter, so the network header starts after it.
 */
static __always_inline __u64 outgoing_bytes(struct __sk_buff *skb, struct tcpdelay_counters *totals)
{
	__u32 zero = 0;
	const struct tcpdelay_accounting *config = bpf_map_lookup_elem(&accounting, &zero);
	__u32 offset = 0;

	if (config == NULL)
		goto unsupported;
	if (!config->enabled)
		return skb->len;
	/* A post-qdisc GSO aggregate needs offload/header semantics unavailable here. */
	if (skb->gso_size || skb->vlan_present ||
	    (config->cake.atm_mode != CAKE_ATM_NONE && config->cake.atm_mode != CAKE_ATM_ATM &&
	     config->cake.atm_mode != CAKE_ATM_PTM))
		goto unsupported;
	if (config->hardware_type == ARPHRD_ETHER) {
		struct ethhdr ethernet;

		/* Tagged Ethernet changes at offload boundaries; do not infer its charge. */
		if (bpf_skb_load_bytes(skb, 0, &ethernet, sizeof(ethernet)) < 0 ||
		    ethernet.h_proto == bpf_htons(ETH_P_8021Q) ||
		    ethernet.h_proto == bpf_htons(ETH_P_8021AD))
			goto unsupported;
		if (!config->cake.raw) {
			if (!ip_protocol(ethernet.h_proto) &&
			    ethernet.h_proto != bpf_htons(ETH_P_ARP))
				goto unsupported;
			offset = sizeof(ethernet);
		}
	} else if (!config->cake.raw &&
		   ((config->hardware_type != ARPHRD_NONE && config->hardware_type != ARPHRD_PPP &&
		     config->hardware_type != ARPHRD_RAWIP) ||
		    !ip_protocol(skb->protocol))) {
		goto unsupported;
	}
	if (offset > skb->len)
		goto unsupported;
	return cake_accounted_bytes(&config->cake, skb->len, offset);

unsupported:
	totals->unaccounted_packets++;
	return 0;
}

static __always_inline int load(struct __sk_buff *skb, __u32 offset, void *to, __u32 length)
{
	return bpf_skb_load_bytes_relative(skb, offset, to, length, BPF_HDR_START_NET);
}

/*
 * The index of the timestamp option; -1 for none, -2 when the options end
 * early or are malformed, so nothing may be appended after them.
 */
static __always_inline int timestamp_index(const __u8 *options, __u32 length)
{
	__u32 index = 0;

	/* The layout every common stack uses: NOP, NOP, timestamp. */
	if (length >= 12 && options[0] == TCP_OPTION_NOP && options[1] == TCP_OPTION_NOP &&
	    options[2] == TCP_OPTION_TIMESTAMP && options[3] == TCP_OPTION_TIMESTAMP_LENGTH)
		return 2;
	for (__u32 step = 0; step < TCP_OPTIONS_MAX; step++) {
		__u8 kind;
		__u8 option_length;

		if (index >= length || index >= TCP_OPTIONS_MAX - 1)
			return -1;
		kind = options[index];
		if (kind == TCP_OPTION_END)
			return -2;
		if (kind == TCP_OPTION_NOP) {
			index++;
			continue;
		}
		option_length = options[index + 1];
		if (option_length < 2)
			return -2;
		if (kind == TCP_OPTION_TIMESTAMP && option_length == TCP_OPTION_TIMESTAMP_LENGTH)
			return index + TCP_OPTION_TIMESTAMP_LENGTH <= length ? (int)index : -2;
		index += option_length;
	}
	return -1;
}

/* Returns 1 and fills tsval/tsecr when the options carry a timestamp. */
static __always_inline int
find_timestamp(const __u8 *options, __u32 length, __u32 *tsval, __u32 *tsecr)
{
	int index = timestamp_index(options, length);
	__u32 value;

	if (index < 0 || index > (int)(TCP_OPTIONS_MAX - TCP_OPTION_TIMESTAMP_LENGTH))
		return 0;
	__builtin_memcpy(&value, &options[index + 2], sizeof(value));
	*tsval = bpf_ntohl(value);
	__builtin_memcpy(&value, &options[index + 6], sizeof(value));
	*tsecr = bpf_ntohl(value);
	return 1;
}

/*
 * Fills the flow as (source, destination), the TCP header offset and the IP
 * payload length (TCP header plus data).
 */
static __always_inline int parse_ip(
	struct __sk_buff *skb,
	struct tcpdelay_record_flow *flow,
	__u32 *tcp_offset,
	__u32 *ip_payload
)
{
	if (skb->protocol == bpf_htons(ETH_P_IP)) {
		struct iphdr ip;

		if (load(skb, 0, &ip, sizeof(ip)) < 0 || ip.protocol != IPPROTO_TCP || ip.ihl < 5)
			return -1;
		/* Later fragments carry no TCP header. */
		if ((ip.frag_off & bpf_htons(0x1fff)) != 0)
			return -1;
		/* IPv4-mapped IPv6 (::ffff:a.b.c.d), so both families share a key. */
		flow->local_address[10] = 0xff;
		flow->local_address[11] = 0xff;
		__builtin_memcpy(&flow->local_address[12], &ip.saddr, sizeof(ip.saddr));
		flow->remote_address[10] = 0xff;
		flow->remote_address[11] = 0xff;
		__builtin_memcpy(&flow->remote_address[12], &ip.daddr, sizeof(ip.daddr));
		*tcp_offset = ip.ihl * 4U;
		*ip_payload = bpf_ntohs(ip.tot_len) - *tcp_offset;
		return 0;
	}
	if (skb->protocol == bpf_htons(ETH_P_IPV6)) {
		struct ipv6hdr ip;

		/* Extension headers before TCP are rare enough to skip. */
		if (load(skb, 0, &ip, sizeof(ip)) < 0 || ip.nexthdr != IPPROTO_TCP)
			return -1;
		__builtin_memcpy(flow->local_address, &ip.saddr, sizeof(ip.saddr));
		__builtin_memcpy(flow->remote_address, &ip.daddr, sizeof(ip.daddr));
		*tcp_offset = sizeof(ip);
		*ip_payload = bpf_ntohs(ip.payload_len);
		return 0;
	}
	return -1;
}

static __always_inline void swap_flow(struct tcpdelay_record_flow *flow)
{
	__u8 address[16];
	__u16 port = flow->local_port;

	__builtin_memcpy(address, flow->local_address, sizeof(address));
	__builtin_memcpy(flow->local_address, flow->remote_address, sizeof(address));
	__builtin_memcpy(flow->remote_address, address, sizeof(address));
	flow->local_port = flow->remote_port;
	flow->remote_port = port;
}

/*
 * Whether this is the flow's first outgoing timestamp after the SYN, the
 * client's own, for the injector to learn from.
 */
static __always_inline int outgoing(const struct tcpdelay_record_flow *flow, __u32 tsval, int syn)
{
	struct departure_key key = { .flow = *flow, .tsval = tsval };
	struct flow_state *state = bpf_map_lookup_elem(&flows, flow);
	__u64 now_ns;
	int first;

	if (state != NULL && state->outgoing_tsval == tsval)
		return 0;
	first = !syn && (state == NULL || !state->client_stamped);
	now_ns = bpf_ktime_get_ns();
	if (state == NULL) {
		struct flow_state initial = {
			.outgoing_tsval = tsval,
			.departure_ns = now_ns,
			.client_stamped = !syn,
		};

		bpf_map_update_elem(&flows, flow, &initial, BPF_NOEXIST);
	} else {
		state->outgoing_tsval = tsval;
		if (!syn)
			state->client_stamped = 1U;
		if (now_ns - state->departure_ns < TCPDELAY_SAMPLE_INTERVAL_NS)
			return first;
		state->departure_ns = now_ns;
	}
	bpf_map_update_elem(&departures, &key, &now_ns, BPF_NOEXIST);
	return first;
}

static __always_inline struct tcpdelay_counters *counters_entry(void)
{
	__u32 zero = 0;

	return bpf_map_lookup_elem(&counters, &zero);
}

static __always_inline struct tcpdelay_inject_counters *inject_counters_entry(void)
{
	__u32 zero = 0;

	return bpf_map_lookup_elem(&inject_counters, &zero);
}

/*
 * A server packet on a connection whose injected handshake waits: once it
 * acknowledges the client's first data, the handshake worked.
 */
static __always_inline void inject_check_data(struct flow_state *state, const struct tcphdr *tcp)
{
	struct tcpdelay_inject_counters *totals;

	if (!tcpdelay_inject_data_acknowledged(bpf_ntohl(tcp->ack_seq), state->inject_sequence))
		return;
	state->inject_waiting = 0U;
	totals = inject_counters_entry();
	if (totals != NULL)
		totals->accepted++;
}

/*
 * A reply whose new TSecr echoes a recorded departure is always sampled; any
 * other change of TSval or TSecr at most once per interval, which still feeds
 * the downstream estimate of flows that send no data.
 */
static __always_inline void
incoming(const struct tcpdelay_record_flow *flow, const struct tcphdr *tcp, __u32 tsval, __u32 tsecr)
{
	struct departure_key key = { .flow = *flow, .tsval = tsecr };
	struct flow_state *state = bpf_map_lookup_elem(&flows, flow);
	struct tcpdelay_record *record;
	__u64 *departure_ns = NULL;
	__u64 now_ns = bpf_ktime_get_ns();

	if (state == NULL) {
		struct flow_state initial = {
			.incoming_tsval = tsval,
			.incoming_tsecr = tsecr,
			.sample_ns = now_ns,
		};

		bpf_map_update_elem(&flows, flow, &initial, BPF_NOEXIST);
		if (tsecr != 0)
			departure_ns = bpf_map_lookup_elem(&departures, &key);
	} else {
		/* Rare: only an injected handshake waiting for its first acknowledged data. */
		if (__builtin_expect(state->inject_waiting, 0))
			inject_check_data(state, tcp);
		if (state->incoming_tsval == tsval && state->incoming_tsecr == tsecr)
			return;
		if (state->incoming_tsecr != tsecr && tsecr != 0)
			departure_ns = bpf_map_lookup_elem(&departures, &key);
		state->incoming_tsval = tsval;
		state->incoming_tsecr = tsecr;
		if (departure_ns == NULL && now_ns - state->sample_ns < TCPDELAY_SAMPLE_INTERVAL_NS)
			return;
		state->sample_ns = now_ns;
	}
	record = bpf_ringbuf_reserve(&samples, sizeof(*record), 0);
	if (record == NULL) {
		struct tcpdelay_counters *totals = counters_entry();

		if (totals != NULL)
			totals->ring_full++;
		return;
	}
	record->arrival_ns = now_ns;
	record->departure_ns = departure_ns != NULL ? *departure_ns : 0;
	record->flow = *flow;
	record->tsval = tsval;
	record->tsecr = tsecr;
	/* Userspace drains the ring on each traffic tick and controller run. */
	bpf_ringbuf_submit(record, BPF_RB_NO_WAKEUP);
}

/* The server, for every client. */
static __always_inline struct tcpdelay_inject_server_key
inject_server_key(const struct tcpdelay_record_flow *flow)
{
	struct tcpdelay_inject_server_key key = { .port = flow->remote_port };

	__builtin_memcpy(key.address, flow->remote_address, sizeof(key.address));
	return key;
}

/* A refused timestamped handshake: SYNs to its server are left alone for a day. */
static __always_inline void inject_skip_server(
	const struct tcpdelay_record_flow *flow,
	struct tcpdelay_inject_handshake *handshake,
	struct tcpdelay_inject_counters *totals
)
{
	struct tcpdelay_inject_server_key key = inject_server_key(flow);
	struct tcpdelay_inject_server value = { .rejected_ns = bpf_ktime_get_boot_ns() };

	bpf_map_update_elem(&inject_servers, &key, &value, BPF_ANY);
	handshake->state = TCPDELAY_INJECT_CLOSED;
	totals->skipped++;
}

/*
 * The client's first timestamp on a connection we injected and the server
 * accepted: its clock, injected into its later SYNs. Called only for a flow's
 * first outgoing timestamp.
 */
static __always_inline void inject_learn(const struct tcpdelay_record_flow *flow, __u32 tsval)
{
	struct tcpdelay_inject_handshake *handshake = bpf_map_lookup_elem(&inject_handshakes, flow);
	struct tcpdelay_inject_client_key key = {};
	struct tcpdelay_inject_client client = { .tsval = tsval };

	if (handshake == NULL || handshake->learned || handshake->state != TCPDELAY_INJECT_ECHOED)
		return;
	handshake->learned = 1;
	client.seen_ns = bpf_ktime_get_boot_ns();
	__builtin_memcpy(key.address, flow->local_address, sizeof(key.address));
	bpf_map_update_elem(&inject_clients, &key, &client, BPF_ANY);
}

/*
 * A stalled handshake: the client's clock looks older than the injected TSval.
 * Its next SYNs get STALLED_TSVAL until its clock is learned, usually from this
 * very connection, whose handshake reply carried it.
 */
static __always_inline void inject_stall(const struct tcpdelay_record_flow *flow)
{
	struct tcpdelay_inject_client_key key = {};
	struct tcpdelay_inject_client *known;
	struct tcpdelay_inject_client client = { .stalled = 1U };

	__builtin_memcpy(key.address, flow->local_address, sizeof(key.address));
	known = bpf_map_lookup_elem(&inject_clients, &key);
	if (known != NULL) {
		known->stalled = 1U;
		return;
	}
	client.seen_ns = bpf_ktime_get_boot_ns();
	bpf_map_update_elem(&inject_clients, &key, &client, BPF_ANY);
}

/*
 * The server's answer to a SYN, read by the filter: a SYN-ACK settles an
 * injected handshake, a reset rejects it. Only these packets look the
 * handshake up. echo is the SYN-ACK's TSecr, zero without a timestamp.
 */
static __always_inline void
inject_observe(const struct tcpdelay_record_flow *flow, const struct tcphdr *tcp, __u32 echo)
{
	struct tcpdelay_inject_handshake *handshake = bpf_map_lookup_elem(&inject_handshakes, flow);
	struct tcpdelay_inject_counters *totals;
	struct flow_state *state;

	if (handshake == NULL)
		return;
	totals = inject_counters_entry();
	if (totals == NULL)
		return;
	if (tcp->rst) {
		if (tcpdelay_inject_server_reset_rejects(handshake))
			inject_skip_server(flow, handshake, totals);
		return;
	}
	switch (tcpdelay_inject_classify_answer(handshake, echo == handshake->tsval)) {
	case TCPDELAY_INJECT_ANSWER_ECHOED:
		handshake->state = TCPDELAY_INJECT_ECHOED;
		/* Counted as accepted once the server acknowledges the client's data. */
		state = bpf_map_lookup_elem(&flows, flow);
		if (state != NULL) {
			state->inject_waiting = 1U;
			state->inject_sequence = handshake->sequence;
		}
		break;
	case TCPDELAY_INJECT_ANSWER_DECLINED:
		/* The server does not take timestamps: leave its SYNs alone. */
		inject_skip_server(flow, handshake, totals);
		break;
	case TCPDELAY_INJECT_ANSWER_REJECTED:
		inject_skip_server(flow, handshake, totals);
		break;
	case TCPDELAY_INJECT_ANSWER_STALLED:
		handshake->state = TCPDELAY_INJECT_CLOSED;
		inject_stall(flow);
		totals->stalled++;
		break;
	case TCPDELAY_INJECT_ANSWER_NONE:
		break;
	}
}

SEC("socket")
int tcpdelay(struct __sk_buff *skb)
{
	/* Only outgoing packets are counted, so only they look the counters up. */
	struct tcpdelay_counters *totals = NULL;
	__u64 charge_bytes = 0;
	struct tcpdelay_record_flow flow = {};
	__u8 options[TCP_OPTIONS_MAX] = {};
	struct tcphdr tcp;
	/*
	 * Initialized although parse_ip() sets them on success: otherwise clang
	 * may spill an undefined value from a register a helper call clobbered,
	 * and the verifier rejects the read.
	 */
	__u32 tcp_offset = 0;
	__u32 ip_payload = 0;
	__u32 options_length;
	__u32 tsval = 0;
	__u32 tsecr = 0;
	/* Only IPv6 connections are injected, so only they look a handshake up. */
	int injectable = skb->protocol == bpf_htons(ETH_P_IPV6);
	int found = 0;
	int answer;

	if (skb->pkt_type == PACKET_OUTGOING) {
		totals = counters_entry();
		/* For the verifier; the array's one entry exists from creation. */
		if (totals == NULL)
			return 0;
		charge_bytes = outgoing_bytes(skb, totals);
		totals->upload_bytes += charge_bytes;
	}
	if (parse_ip(skb, &flow, &tcp_offset, &ip_payload) < 0 ||
	    load(skb, tcp_offset, &tcp, sizeof(tcp)) < 0) {
		return 0;
	}
	/* A pure ACK carries no data and none of SYN, FIN or RST. */
	if (totals != NULL && ip_payload == tcp.doff * 4U && tcp.ack && !tcp.syn && !tcp.fin &&
	    !tcp.rst) {
		totals->ack_bytes += charge_bytes;
	}
	flow.local_port = tcp.source;
	flow.remote_port = tcp.dest;
	options_length = tcp.doff * 4U;
	if (options_length >= sizeof(tcp) + TCP_OPTION_TIMESTAMP_LENGTH) {
		options_length -= sizeof(tcp);
		if (options_length > TCP_OPTIONS_MAX)
			options_length = TCP_OPTIONS_MAX;
		found = load(skb, tcp_offset + sizeof(tcp), options, options_length) == 0 &&
			find_timestamp(options, options_length, &tsval, &tsecr);
	}
	if (skb->pkt_type == PACKET_OUTGOING) {
		if (found && outgoing(&flow, tsval, tcp.syn) && injectable)
			inject_learn(&flow, tsval);
		return 0;
	}
	/* A SYN-ACK or reset may answer an injected SYN, with or without a timestamp. */
	answer = injectable && (tcp.rst || (tcp.syn && tcp.ack));
	if (!found && !answer)
		return 0;
	swap_flow(&flow);
	if (answer)
		inject_observe(&flow, &tcp, found ? tsecr : 0U);
	if (found)
		incoming(&flow, &tcp, tsval, tsecr);
	return 0;
}

/* An outgoing SYN or reset, copied from the packet for the injector. */
struct segment {
	struct tcpdelay_record_flow flow;
	struct tcphdr tcp;
	__u32 network_offset;
	__u32 tcp_offset;
	/* The whole IP packet: header, TCP header and data. */
	__u32 ip_bytes;
	__u32 options_bytes;
};

/* Fills the segment as (source, destination); -1 for anything but TCP over IPv6. */
static __always_inline int
inject_parse(struct __sk_buff *skb, __u32 network_offset, struct segment *segment)
{
	struct ipv6hdr ip;

	segment->network_offset = network_offset;
	/* Extension headers before TCP are rare enough to skip. */
	if (bpf_skb_load_bytes(skb, network_offset, &ip, sizeof(ip)) < 0 ||
	    ip.nexthdr != IPPROTO_TCP)
		return -1;
	__builtin_memcpy(segment->flow.local_address, &ip.saddr, sizeof(ip.saddr));
	__builtin_memcpy(segment->flow.remote_address, &ip.daddr, sizeof(ip.daddr));
	segment->tcp_offset = network_offset + sizeof(ip);
	segment->ip_bytes = sizeof(ip) + bpf_ntohs(ip.payload_len);
	if (bpf_skb_load_bytes(skb, segment->tcp_offset, &segment->tcp, sizeof(segment->tcp)) < 0 ||
	    segment->tcp.doff < 5)
		return -1;
	segment->flow.local_port = segment->tcp.source;
	segment->flow.remote_port = segment->tcp.dest;
	segment->options_bytes = segment->tcp.doff * 4U - TCP_HEADER_BYTES;
	return 0;
}

/*
 * Appends NOP, NOP, TS(tsval, 0) to a SYN without data and updates the
 * lengths and checksums incrementally. The helpers keep a partial checksum
 * (a packet whose checksum the NIC completes) correct as well.
 */
static __always_inline int
append_timestamp(struct __sk_buff *skb, const struct segment *segment, __u32 tsval)
{
	__u32 added[3] = { bpf_htonl(0x0101080aU), bpf_htonl(tsval), 0 };
	__u32 header_bytes = segment->tcp.doff * 4U;
	__u32 end = segment->tcp_offset + header_bytes;
	__u32 checksum = segment->tcp_offset + TCP_CHECKSUM_OFFSET;
	__u16 old_word;
	__u16 new_word;
	__be16 payload;
	__s64 diff;

	if (bpf_skb_change_tail(
		    skb,
		    segment->network_offset + segment->ip_bytes + INJECT_ADDED_BYTES,
		    0
	    ) < 0 ||
	    bpf_skb_store_bytes(skb, end, added, sizeof(added), 0) < 0 ||
	    bpf_skb_load_bytes(
		    skb,
		    segment->tcp_offset + TCP_FLAGS_OFFSET,
		    &old_word,
		    sizeof(old_word)
	    ) < 0)
		return -1;
	/* The data offset is the high nibble of this word's first byte. */
	new_word = old_word;
	((__u8 *)&new_word)[0] = (__u8)((((header_bytes + INJECT_ADDED_BYTES) / 4U) << 4) |
					(((__u8 *)&old_word)[0] & 0x0fU));
	if (bpf_skb_store_bytes(
		    skb,
		    segment->tcp_offset + TCP_FLAGS_OFFSET,
		    &new_word,
		    sizeof(new_word),
		    0
	    ) < 0)
		return -1;
	/* TCP: the flags word, the pseudo-header length and the new bytes. */
	bpf_l4_csum_replace(skb, checksum, old_word, new_word, 2);
	bpf_l4_csum_replace(
		skb,
		checksum,
		bpf_htons(header_bytes),
		bpf_htons(header_bytes + INJECT_ADDED_BYTES),
		BPF_F_PSEUDO_HDR | 2
	);
	diff = bpf_csum_diff(NULL, 0, added, sizeof(added), 0);
	bpf_l4_csum_replace(skb, checksum, 0, (__u32)diff, 0);
	/* IPv6 has no header checksum; only its payload length grows. */
	payload = bpf_htons(segment->ip_bytes - sizeof(struct ipv6hdr) + INJECT_ADDED_BYTES);
	return bpf_skb_store_bytes(
		skb,
		segment->network_offset + IPV6_LENGTH_OFFSET,
		&payload,
		sizeof(payload),
		0
	);
}

/* The TSval to inject: tcpdelay_inject_client_tsval() for the SYN's client. */
static __always_inline __u32 inject_tsval(const struct tcpdelay_record_flow *flow, __u64 now_ns)
{
	struct tcpdelay_inject_client_key key = {};

	__builtin_memcpy(key.address, flow->local_address, sizeof(key.address));
	return tcpdelay_inject_client_tsval(bpf_map_lookup_elem(&inject_clients, &key), now_ns);
}

/*
 * Whether the SYN's options might hold a timestamp: its kind and length bytes
 * (8, 10) anywhere. A false "might" only skips injecting one SYN; a timestamp
 * is never missed. The full parser's loop, inlined with the rest of the
 * injector, would exceed the verifier's limit, and calls between programs or
 * functions are not compiled by every BPF JIT.
 */
static __always_inline int inject_options_may_timestamp(const __u8 *options, __u32 length)
{
	for (__u32 i = 0; i + 1U < TCP_OPTIONS_MAX; i++) {
		if (i + 1U >= length)
			return 0;
		if (options[i] == TCP_OPTION_TIMESTAMP &&
		    options[i + 1U] == TCP_OPTION_TIMESTAMP_LENGTH)
			return 1;
	}
	return 0;
}

/*
 * Whether a SYN may get a timestamp: no timestamp in its options, no data and
 * room for 12 more bytes.
 */
static __always_inline int inject_syn_eligible(struct __sk_buff *skb, const struct segment *segment)
{
	__u8 options[TCP_OPTIONS_MAX] = {};

	if (segment->options_bytes > TCP_OPTIONS_MAX ||
	    (segment->options_bytes > 0 &&
	     (bpf_skb_load_bytes(
		      skb,
		      segment->tcp_offset + TCP_HEADER_BYTES,
		      options,
		      segment->options_bytes
	      ) < 0 ||
	      inject_options_may_timestamp(options, segment->options_bytes))) ||
	    segment->ip_bytes !=
		    segment->tcp_offset - segment->network_offset + segment->tcp.doff * 4U ||
	    segment->tcp.doff * 4U + INJECT_ADDED_BYTES > TCP_HEADER_MAX_BYTES)
		return 0;
	return 1;
}

/* A SYN without a timestamp: inject, or settle a resent one. */
static __always_inline void inject_new_syn(
	struct __sk_buff *skb,
	struct segment *segment,
	struct tcpdelay_inject_counters *totals
)
{
	struct tcpdelay_inject_handshake *handshake;
	struct tcpdelay_inject_handshake fresh = {};
	struct tcpdelay_inject_server_key key;
	__u32 sequence = bpf_ntohl(segment->tcp.seq);
	__u64 now_ns;
	__u32 tsval;

	handshake = bpf_map_lookup_elem(&inject_handshakes, &segment->flow);
	switch (tcpdelay_inject_classify_syn(handshake, sequence)) {
	case TCPDELAY_INJECT_SYN_RETRY:
		if (handshake != NULL)
			handshake->state = TCPDELAY_INJECT_RETRIED;
		return;
	case TCPDELAY_INJECT_SYN_REJECTED:
		/* The answer never reached the client's stack: skip the server. */
		if (handshake != NULL)
			inject_skip_server(&segment->flow, handshake, totals);
		return;
	case TCPDELAY_INJECT_SYN_PASS:
		return;
	case TCPDELAY_INJECT_SYN_NEW:
		break;
	}
	now_ns = bpf_ktime_get_boot_ns();
	key = inject_server_key(&segment->flow);
	if (tcpdelay_inject_server_skipped(bpf_map_lookup_elem(&inject_servers, &key), now_ns)) {
		totals->skipped++;
		return;
	}
	tsval = inject_tsval(&segment->flow, now_ns);
	if (append_timestamp(skb, segment, tsval) < 0)
		return;
	fresh.sequence = sequence;
	fresh.tsval = tsval;
	fresh.state = TCPDELAY_INJECT_SENT;
	bpf_map_update_elem(&inject_handshakes, &segment->flow, &fresh, BPF_ANY);
	totals->injected++;
}

/* A SYN or a client reset, copied out of the packet with helpers. */
static __always_inline void inject_outgoing(struct __sk_buff *skb, __u32 network_offset)
{
	struct segment segment = {};
	struct tcpdelay_inject_counters *totals = inject_counters_entry();
	struct tcpdelay_inject_handshake *handshake;

	if (totals == NULL || inject_parse(skb, network_offset, &segment) < 0)
		return;
	if (segment.tcp.rst) {
		handshake = bpf_map_lookup_elem(&inject_handshakes, &segment.flow);
		if (tcpdelay_inject_client_reset_rejects(handshake, bpf_ntohl(segment.tcp.seq)) &&
		    handshake != NULL)
			inject_skip_server(&segment.flow, handshake, totals);
		return;
	}
	if (inject_syn_eligible(skb, &segment))
		inject_new_syn(skb, &segment, totals);
}

/*
 * tcx egress on the upload interface, before the root qdisc. Every packet is
 * checked in place, without copies or helper calls; only IPv6 SYNs and resets
 * go on.
 * Returns TCX_NEXT, so tc filters after it still run.
 */
SEC("tcx/egress")
int inject_egress(struct __sk_buff *skb)
{
	__u32 zero = 0;
	const struct tcpdelay_inject_settings *config;
	__u8 *data = (__u8 *)(long)skb->data;
	__u8 *data_end = (__u8 *)(long)skb->data_end;
	struct ipv6hdr *ip;
	struct tcphdr *tcp;
	__u32 network_offset;

	/*
	 * IPv4 is left alone, before any lookup: behind NAT, one TSval would have
	 * to suit every client.
	 */
	if (skb->protocol != bpf_htons(ETH_P_IPV6))
		return TCX_NEXT;
	config = bpf_map_lookup_elem(&inject_settings, &zero);
	if (config == NULL || config->network_offset > ETH_HLEN)
		return TCX_NEXT;
	network_offset = config->network_offset;
	ip = (struct ipv6hdr *)(data + network_offset);
	if ((__u8 *)(ip + 1) > data_end || ip->nexthdr != IPPROTO_TCP)
		return TCX_NEXT;
	tcp = (struct tcphdr *)(ip + 1);
	if ((__u8 *)(tcp + 1) > data_end || (!tcp->rst && (!tcp->syn || tcp->ack)))
		return TCX_NEXT;
	inject_outgoing(skb, network_offset);
	return TCX_NEXT;
}

char LICENSE[] SEC("license") = "GPL";
