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

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} stream_epoch_v1 SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, TCPDELAY_STREAM_LOSSES);
	__type(key, __u32);
	__type(value, __u32);
} stream_losses SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} stream_fault SEC(".maps");

/* Racy updates from several CPUs only cost an extra or a skipped sample. */
struct flow_state {
	__u64 departure_ns;
	__u64 sample_ns;
	__u32 outgoing_tsval;
	__u32 incoming_tsval;
	__u32 incoming_tsecr;
	__u32 epoch;
};

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, TCPDELAY_DEPARTURES);
	__type(key, struct departure_key);
	__type(value, __u32);
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

/* Packet taps push the Ethernet header back before running a RAW socket filter. */
static __always_inline int network_offset(struct __sk_buff *skb, __u32 hardware_type, __u32 *offset)
{
	struct ethhdr ethernet;
	__be16 protocol;

	*offset = 0;
	if (hardware_type == ARPHRD_NONE || hardware_type == ARPHRD_PPP ||
	    hardware_type == ARPHRD_RAWIP) {
		return skb->protocol == bpf_htons(ETH_P_IP) ||
		       skb->protocol == bpf_htons(ETH_P_IPV6);
	}
	if (hardware_type != ARPHRD_ETHER ||
	    bpf_skb_load_bytes(skb, 0, &ethernet, sizeof(ethernet)) < 0)
		return 0;
	*offset = sizeof(ethernet);
	protocol = ethernet.h_proto;
	return protocol == bpf_htons(ETH_P_IP) || protocol == bpf_htons(ETH_P_IPV6) ||
	       protocol == bpf_htons(ETH_P_ARP);
}

/* Both counters use this one charge. Unsupported traffic leaves timing available. */
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
	/* Tagged Ethernet changes at offload boundaries; do not infer its charge. */
	if (config->hardware_type == ARPHRD_ETHER) {
		struct ethhdr ethernet;

		if (bpf_skb_load_bytes(skb, 0, &ethernet, sizeof(ethernet)) < 0 ||
		    ethernet.h_proto == bpf_htons(ETH_P_8021Q) ||
		    ethernet.h_proto == bpf_htons(ETH_P_8021AD))
			goto unsupported;
	}
	if (!config->cake.raw && !network_offset(skb, config->hardware_type, &offset))
		goto unsupported;
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

/* Returns 1 and fills tsval/tsecr when the options carry a timestamp. */
static __always_inline int
find_timestamp(const __u8 *options, __u32 length, __u32 *tsval, __u32 *tsecr)
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

static __always_inline struct tcpdelay_counters *counters_entry(void)
{
	__u32 zero = 0;

	return bpf_map_lookup_elem(&counters, &zero);
}

static __always_inline int stream_usable(__u32 epoch)
{
	__u32 zero = 0U;
	__u32 *fault = bpf_map_lookup_elem(&stream_fault, &zero);
	__u32 *loss = bpf_map_lookup_elem(&stream_losses, &epoch);

	return epoch != 0U && epoch <= TCPDELAY_STREAM_EPOCHS && fault != NULL && *fault == 0U &&
	       loss == NULL;
}

static __always_inline void stream_lost(__u32 epoch)
{
	__u32 marker = 1U;
	__u32 zero = 0U;

	if (bpf_map_update_elem(&stream_losses, &epoch, &marker, BPF_NOEXIST) < 0 &&
	    bpf_map_lookup_elem(&stream_losses, &epoch) == NULL) {
		__u32 *fault = bpf_map_lookup_elem(&stream_fault, &zero);

		if (fault != NULL)
			*fault = 1U;
	}
}

static __always_inline void emit_record(
	const struct tcpdelay_record_flow *flow,
	__u64 now,
	__u32 tsval,
	__u32 tsecr,
	__u32 sequence,
	__u32 acknowledgment,
	__u32 metadata,
	__u32 epoch
)
{
	struct tcpdelay_record *record = bpf_ringbuf_reserve(&samples, sizeof(*record), 0);

	if (record == NULL) {
		struct tcpdelay_counters *totals = counters_entry();

		if (totals != NULL)
			totals->ring_full++;
		stream_lost(epoch);
		return;
	}
	record->arrival_ns = now;
	record->flow = *flow;
	record->tsval = tsval;
	record->tsecr = tsecr;
	record->sequence = sequence;
	record->acknowledgment = acknowledgment;
	record->metadata = metadata | (TCPDELAY_STREAM_VERSION << TCPDELAY_STREAM_VERSION_SHIFT) |
			   (epoch << TCPDELAY_STREAM_EPOCH_SHIFT);
	bpf_ringbuf_submit(record, BPF_RB_NO_WAKEUP);
}

static __always_inline void outgoing(
	const struct tcpdelay_record_flow *flow,
	__u32 tsval,
	__u32 sequence,
	__u32 acknowledgment,
	__u32 flags,
	__u32 epoch,
	int has_timestamp,
	__u64 now
)
{
	struct departure_key key = { .flow = *flow, .tsval = tsval };
	struct flow_state *state;
	struct flow_state initial = { .epoch = epoch };
	__u32 metadata = flags | TCPDELAY_STREAM_OUTGOING;

	if (has_timestamp)
		metadata |= TCPDELAY_STREAM_HAS_TIMESTAMP;
	if (!has_timestamp || !stream_usable(epoch))
		return;
	state = bpf_map_lookup_elem(&flows, flow);
	if (state == NULL || state->epoch != epoch) {
		initial.outgoing_tsval = tsval;
		initial.departure_ns = now;
		(void)bpf_map_update_elem(&flows, flow, &initial, BPF_ANY);
	} else {
		if (state->outgoing_tsval == tsval)
			return;
		state->outgoing_tsval = tsval;
		if (now - state->departure_ns < TCPDELAY_SAMPLE_INTERVAL_NS)
			return;
		state->departure_ns = now;
	}
	(void)bpf_map_update_elem(&departures, &key, &epoch, BPF_ANY);
	emit_record(flow, now, tsval, 0U, sequence, acknowledgment, metadata, epoch);
}

static __always_inline void incoming(
	const struct tcpdelay_record_flow *flow,
	__u32 tsval,
	__u32 tsecr,
	__u32 sequence,
	__u32 acknowledgment,
	__u32 flags,
	__u32 epoch,
	__u64 now
)
{
	struct departure_key key = { .flow = *flow, .tsval = tsecr };
	struct flow_state *state;
	struct flow_state initial = { .epoch = epoch };
	__u32 *departure = NULL;
	__u32 departure_epoch = 0U;
	__u32 metadata = flags | TCPDELAY_STREAM_HAS_TIMESTAMP;
	int matched = 0;

	if (!stream_usable(epoch))
		return;
	state = bpf_map_lookup_elem(&flows, flow);
	if (state == NULL || state->epoch != epoch) {
		initial.incoming_tsval = tsval;
		initial.incoming_tsecr = tsecr;
		initial.sample_ns = now;
		(void)bpf_map_update_elem(&flows, flow, &initial, BPF_ANY);
	} else {
		__u32 previous_tsval = state->incoming_tsval;
		__u32 previous_tsecr = state->incoming_tsecr;
		__u64 previous_sample_ns = state->sample_ns;

		if (previous_tsval == tsval && previous_tsecr == tsecr)
			return;
		if (previous_tsecr != tsecr && tsecr != 0U)
			departure = bpf_map_lookup_elem(&departures, &key);
		if (departure != NULL)
			departure_epoch = *departure;
		matched = departure_epoch == epoch;
		state = bpf_map_lookup_elem(&flows, flow);
		if (state == NULL || state->epoch != epoch)
			return;
		state->incoming_tsval = tsval;
		state->incoming_tsecr = tsecr;
		if (!matched && now - previous_sample_ns < TCPDELAY_SAMPLE_INTERVAL_NS)
			return;
		state->sample_ns = now;
	}
	emit_record(flow, now, tsval, tsecr, sequence, acknowledgment, metadata, epoch);
}

static __always_inline void lifecycle_event(
	const struct tcpdelay_record_flow *flow,
	__u32 tsval,
	__u32 tsecr,
	__u32 sequence,
	__u32 acknowledgment,
	__u32 flags,
	__u32 epoch,
	int outgoing_packet,
	int has_timestamp,
	__u64 now
)
{
	__u32 metadata = flags | (outgoing_packet ? TCPDELAY_STREAM_OUTGOING : 0U);

	if (has_timestamp)
		metadata |= TCPDELAY_STREAM_HAS_TIMESTAMP;
	if (!stream_usable(epoch))
		return;
	emit_record(flow, now, tsval, tsecr, sequence, acknowledgment, metadata, epoch);
}

SEC("socket")
int tcpdelay(struct __sk_buff *skb)
{
	/* Only outgoing packets are counted, so only they look the counters up. */
	struct tcpdelay_counters *totals = NULL;
	__u64 charge = 0;
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
	__u32 tsval = 0U;
	__u32 tsecr = 0U;
	__u32 sequence;
	__u32 acknowledgment;
	__u32 epoch_key = 0U;
	__u32 *epoch_value;
	__u32 flags = 0U;
	int has_timestamp = 0;
	int outgoing_packet = skb->pkt_type == PACKET_OUTGOING;
	__u64 packet_ns = bpf_ktime_get_ns();
	int lifecycle;

	if (skb->pkt_type == PACKET_OUTGOING) {
		totals = counters_entry();
		/* For the verifier; the array's one entry exists from creation. */
		if (totals == NULL)
			return 0;
		charge = outgoing_bytes(skb, totals);
		totals->upload_bytes += charge;
	}
	if (parse_ip(skb, &flow, &tcp_offset, &ip_payload) < 0 ||
	    load(skb, tcp_offset, &tcp, sizeof(tcp)) < 0) {
		return 0;
	}
	/* A pure ACK carries no data and none of SYN, FIN or RST. */
	if (totals != NULL && ip_payload == tcp.doff * 4U && tcp.ack && !tcp.syn && !tcp.fin &&
	    !tcp.rst) {
		totals->ack_bytes += charge;
	}
	sequence = bpf_ntohl(tcp.seq);
	acknowledgment = bpf_ntohl(tcp.ack_seq);
	if (tcp.syn)
		flags |= TCPDELAY_TCP_SYN;
	if (tcp.fin)
		flags |= TCPDELAY_TCP_FIN;
	if (tcp.rst)
		flags |= TCPDELAY_TCP_RST;
	if (tcp.ack)
		flags |= TCPDELAY_TCP_ACK;
	if (tcp.psh)
		flags |= TCPDELAY_TCP_PSH;
	if (tcp.urg)
		flags |= TCPDELAY_TCP_URG;
	if (tcp.ece)
		flags |= TCPDELAY_TCP_ECE;
	if (tcp.cwr)
		flags |= TCPDELAY_TCP_CWR;
	lifecycle = tcp.syn || tcp.fin || tcp.rst;
	options_length = tcp.doff * 4U;
	if (options_length >= sizeof(tcp) + TCP_OPTION_TIMESTAMP_LENGTH) {
		options_length -= sizeof(tcp);
		if (options_length > TCP_OPTIONS_MAX)
			options_length = TCP_OPTIONS_MAX;
		if (load(skb, tcp_offset + sizeof(tcp), options, options_length) == 0)
			has_timestamp = find_timestamp(options, options_length, &tsval, &tsecr);
	}
	flow.local_port = tcp.source;
	flow.remote_port = tcp.dest;
	if (!outgoing_packet)
		swap_flow(&flow);
	if (!lifecycle && !has_timestamp)
		return 0;
	epoch_value = bpf_map_lookup_elem(&stream_epoch_v1, &epoch_key);
	if (epoch_value == NULL)
		return 0;
	{
		__u32 epoch = *epoch_value;

		if (lifecycle) {
			lifecycle_event(
				&flow,
				tsval,
				tsecr,
				sequence,
				acknowledgment,
				flags,
				epoch,
				outgoing_packet,
				has_timestamp,
				packet_ns
			);
			return 0;
		}
		if (!has_timestamp)
			return 0;
		if (outgoing_packet)
			outgoing(
				&flow,
				tsval,
				sequence,
				acknowledgment,
				flags,
				epoch,
				has_timestamp,
				packet_ns
			);
		else
			incoming(
				&flow,
				tsval,
				tsecr,
				sequence,
				acknowledgment,
				flags,
				epoch,
				packet_ns
			);
	}
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
