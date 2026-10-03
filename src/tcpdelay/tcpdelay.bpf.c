// SPDX-License-Identifier: GPL-2.0-only
/*
 * Socket filter for an AF_PACKET socket on the upload (WAN) interface. Packet
 * taps see outgoing packets after the root qdisc and incoming packets before
 * the ingress redirect to the IFB, so the delays measured from these times
 * exclude our own CAKE queues and include the ISP's.
 *
 * Outgoing: count all bytes and pure-ACK bytes, and record when each new TSval
 * of a flow first left.
 * Incoming: emit the remote TSval and the departure of the TSval it echoes
 * (TSecr), once per change of either; the first packet after a change carries
 * the minimum delay of its group, which is all the estimator keeps.
 *
 * The filter always returns 0, so no packet is copied to the socket.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/if_packet.h>
#include <linux/tcp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#include "tcpdelay/record.h"

#define TCP_OPTIONS_MAX 40U
#define TCP_OPTION_END 0U
#define TCP_OPTION_NOP 1U
#define TCP_OPTION_TIMESTAMP 8U
#define TCP_OPTION_TIMESTAMP_LENGTH 10U

struct departure_key {
	struct tcpdelay_record_flow flow;
	__u32 tsval;
};

/* Racy updates from several CPUs only cost an extra or a skipped sample. */
struct flow_state {
	__u32 outgoing_tsval;
	__u32 incoming_tsval;
	__u32 incoming_tsecr;
	__u32 reserved;
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

static __always_inline int load(struct __sk_buff *skb, __u32 offset, void *to, __u32 length)
{
	return bpf_skb_load_bytes_relative(skb, offset, to, length, BPF_HDR_START_NET);
}

/* Returns 1 and fills tsval/tsecr when the options carry a timestamp. */
static __always_inline int find_timestamp(const __u8 *options, __u32 length, __u32 *tsval,
					  __u32 *tsecr)
{
	__u32 index = 0;
	__u32 value;

	/* The layout every common stack uses: NOP, NOP, timestamp. */
	if (length >= 12 && options[0] == TCP_OPTION_NOP && options[1] == TCP_OPTION_NOP &&
	    options[2] == TCP_OPTION_TIMESTAMP && options[3] == TCP_OPTION_TIMESTAMP_LENGTH) {
		index = 2;
		goto found;
	}
	for (__u32 step = 0; step < TCP_OPTIONS_MAX; step++) {
		__u8 kind;
		__u8 option_length;

		if (index >= length || index >= TCP_OPTIONS_MAX - 1)
			return 0;
		kind = options[index];
		if (kind == TCP_OPTION_END)
			return 0;
		if (kind == TCP_OPTION_NOP) {
			index++;
			continue;
		}
		option_length = options[index + 1];
		if (option_length < 2)
			return 0;
		if (kind == TCP_OPTION_TIMESTAMP && option_length == TCP_OPTION_TIMESTAMP_LENGTH) {
			if (index + TCP_OPTION_TIMESTAMP_LENGTH > length)
				return 0;
			goto found;
		}
		index += option_length;
	}
	return 0;

found:
	if (index > TCP_OPTIONS_MAX - TCP_OPTION_TIMESTAMP_LENGTH)
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
static __always_inline int parse_ip(struct __sk_buff *skb, struct tcpdelay_record_flow *flow,
				    __u32 *tcp_offset, __u32 *ip_payload)
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

static __always_inline void outgoing(const struct tcpdelay_record_flow *flow, __u32 tsval)
{
	struct departure_key key = { .flow = *flow, .tsval = tsval };
	struct flow_state *state = bpf_map_lookup_elem(&flows, flow);
	__u64 now = bpf_ktime_get_ns();

	if (state == NULL) {
		struct flow_state initial = { .outgoing_tsval = tsval };

		bpf_map_update_elem(&flows, flow, &initial, BPF_NOEXIST);
	} else if (state->outgoing_tsval != tsval) {
		state->outgoing_tsval = tsval;
	} else {
		return;
	}
	bpf_map_update_elem(&departures, &key, &now, BPF_NOEXIST);
}

static __always_inline void incoming(struct tcpdelay_counters *totals,
				     const struct tcpdelay_record_flow *flow, __u32 tsval,
				     __u32 tsecr)
{
	struct departure_key key = { .flow = *flow, .tsval = tsecr };
	struct flow_state *state = bpf_map_lookup_elem(&flows, flow);
	struct tcpdelay_record *record;
	__u64 *departure;

	if (state == NULL) {
		struct flow_state initial = { .incoming_tsval = tsval, .incoming_tsecr = tsecr };

		bpf_map_update_elem(&flows, flow, &initial, BPF_NOEXIST);
	} else if (state->incoming_tsval != tsval || state->incoming_tsecr != tsecr) {
		state->incoming_tsval = tsval;
		state->incoming_tsecr = tsecr;
	} else {
		return;
	}
	departure = tsecr != 0 ? bpf_map_lookup_elem(&departures, &key) : NULL;
	record = bpf_ringbuf_reserve(&samples, sizeof(*record), 0);
	if (record == NULL) {
		totals->ring_full++;
		return;
	}
	record->arrival_ns = bpf_ktime_get_ns();
	record->departure_ns = departure != NULL ? *departure : 0;
	record->flow = *flow;
	record->tsval = tsval;
	record->tsecr = tsecr;
	/* Userspace drains the ring on its own timer. */
	bpf_ringbuf_submit(record, BPF_RB_NO_WAKEUP);
}

SEC("socket")
int tcpdelay(struct __sk_buff *skb)
{
	__u32 zero = 0;
	struct tcpdelay_counters *totals = bpf_map_lookup_elem(&counters, &zero);
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
	__u32 tsval;
	__u32 tsecr;

	/* For the verifier; the array's one entry exists from creation. */
	if (totals == NULL)
		return 0;
	if (skb->pkt_type == PACKET_OUTGOING)
		totals->upload_bytes += skb->len;
	if (parse_ip(skb, &flow, &tcp_offset, &ip_payload) < 0 ||
	    load(skb, tcp_offset, &tcp, sizeof(tcp)) < 0) {
		return 0;
	}
	/* A pure ACK carries no data and none of SYN, FIN or RST. */
	if (skb->pkt_type == PACKET_OUTGOING && ip_payload == tcp.doff * 4U && tcp.ack &&
	    !tcp.syn && !tcp.fin && !tcp.rst) {
		totals->ack_bytes += skb->len;
	}
	options_length = tcp.doff * 4U;
	if (options_length < sizeof(tcp) + TCP_OPTION_TIMESTAMP_LENGTH)
		return 0;
	options_length -= sizeof(tcp);
	if (options_length > TCP_OPTIONS_MAX)
		options_length = TCP_OPTIONS_MAX;
	if (load(skb, tcp_offset + sizeof(tcp), options, options_length) < 0 ||
	    !find_timestamp(options, options_length, &tsval, &tsecr)) {
		return 0;
	}
	flow.local_port = tcp.source;
	flow.remote_port = tcp.dest;
	if (skb->pkt_type == PACKET_OUTGOING) {
		outgoing(&flow, tsval);
	} else {
		swap_flow(&flow);
		incoming(totals, &flow, tsval, tsecr);
	}
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
