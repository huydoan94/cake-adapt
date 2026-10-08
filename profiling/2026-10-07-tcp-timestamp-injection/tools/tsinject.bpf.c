// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experiment, not part of cake-adapt: give TCP connections from clients that
 * do not negotiate timestamps (Windows) a server-side timestamp clock.
 *
 * LAN ingress: a SYN without a timestamp option and without data gets
 * NOP, NOP, TS(TSval = TSINJECT_MARKER, TSecr = 0) appended to its options.
 * A server that supports timestamps then sends TSval on every packet, and the
 * cake-adapt tap on the WAN sees them. The client never sends a timestamp, so
 * the server keeps echoing the marker as TSecr.
 *
 * LAN egress: a packet whose TSecr is the marker belongs to such a connection;
 * its timestamp option is overwritten with NOPs, so the client sees the
 * connection it asked for.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/pkt_cls.h>
#include <linux/tcp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#define TSINJECT_MARKER 0xca4ead01U
#define ETHERNET_BYTES 14U
#define TCP_HEADER_BYTES 20U
#define TCP_HEADER_MAX_BYTES 60U
#define TCP_OPTIONS_MAX 40U
#define TCP_OPTION_END 0U
#define TCP_OPTION_NOP 1U
#define TCP_OPTION_TIMESTAMP 8U
#define TCP_OPTION_TIMESTAMP_LENGTH 10U
#define ADDED_BYTES 12U
#define CHECKSUM_OFFSET 16U
#define FLAGS_OFFSET 12U

struct packet {
	__u32 tcp_offset;
	/* The whole IP packet: header, TCP header and data. */
	__u32 ip_bytes;
	int ipv6;
};

static __always_inline int parse(struct __sk_buff *skb, struct packet *packet)
{
	__be16 protocol;

	if (bpf_skb_load_bytes(skb, 12, &protocol, sizeof(protocol)) < 0)
		return -1;
	if (protocol == bpf_htons(ETH_P_IP)) {
		struct iphdr ip;

		if (bpf_skb_load_bytes(skb, ETHERNET_BYTES, &ip, sizeof(ip)) < 0 ||
		    ip.protocol != IPPROTO_TCP || ip.ihl < 5 || (ip.frag_off & bpf_htons(0x3fff)))
			return -1;
		packet->tcp_offset = ETHERNET_BYTES + ip.ihl * 4U;
		packet->ip_bytes = bpf_ntohs(ip.tot_len);
		packet->ipv6 = 0;
		return 0;
	}
	if (protocol == bpf_htons(ETH_P_IPV6)) {
		struct ipv6hdr ip;

		if (bpf_skb_load_bytes(skb, ETHERNET_BYTES, &ip, sizeof(ip)) < 0 ||
		    ip.nexthdr != IPPROTO_TCP)
			return -1;
		packet->tcp_offset = ETHERNET_BYTES + sizeof(ip);
		packet->ip_bytes = sizeof(ip) + bpf_ntohs(ip.payload_len);
		packet->ipv6 = 1;
		return 0;
	}
	return -1;
}

/*
 * Index of the timestamp option, -1 when there is none, -2 when an end-of-list
 * option means appended options would be ignored.
 */
static __always_inline int find_timestamp(const __u8 *options, __u32 length)
{
	__u32 index = 0;

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

SEC("tc/ingress")
int inject(struct __sk_buff *skb)
{
	__u32 added[3] = {};
	__u8 options[TCP_OPTIONS_MAX] = {};
	struct packet packet;
	struct tcphdr tcp;
	__u32 header_bytes;
	__u32 options_bytes;
	__u32 end;
	__u16 old_word;
	__u16 new_word;
	__s64 diff;

	if (parse(skb, &packet) < 0 ||
	    bpf_skb_load_bytes(skb, packet.tcp_offset, &tcp, sizeof(tcp)) < 0)
		return TC_ACT_OK;
	if (!tcp.syn || tcp.ack || tcp.rst)
		return TC_ACT_OK;
	header_bytes = tcp.doff * 4U;
	if (header_bytes < TCP_HEADER_BYTES || header_bytes + ADDED_BYTES > TCP_HEADER_MAX_BYTES)
		return TC_ACT_OK;
	/* A SYN with data (TCP Fast Open) is left alone. */
	if (packet.ip_bytes != packet.tcp_offset - ETHERNET_BYTES + header_bytes)
		return TC_ACT_OK;
	options_bytes = header_bytes - TCP_HEADER_BYTES;
	if (options_bytes > TCP_OPTIONS_MAX)
		return TC_ACT_OK;
	if (options_bytes > 0 &&
	    (bpf_skb_load_bytes(skb, packet.tcp_offset + TCP_HEADER_BYTES, options, options_bytes) < 0 ||
	     find_timestamp(options, options_bytes) != -1))
		return TC_ACT_OK;

	/* NOP, NOP, kind 8, length 10, TSval = marker, TSecr = 0, in wire order. */
	added[0] = bpf_htonl(0x0101080aU);
	added[1] = bpf_htonl(TSINJECT_MARKER);
	end = packet.tcp_offset + header_bytes;
	if (bpf_skb_change_tail(skb, ETHERNET_BYTES + packet.ip_bytes + ADDED_BYTES, 0) < 0)
		return TC_ACT_OK;
	if (bpf_skb_store_bytes(skb, end, added, sizeof(added), 0) < 0)
		return TC_ACT_SHOT;

	/* Data offset: the high nibble of the first byte of this 16-bit word. */
	if (bpf_skb_load_bytes(skb, packet.tcp_offset + FLAGS_OFFSET, &old_word, sizeof(old_word)) < 0)
		return TC_ACT_SHOT;
	new_word = old_word;
	((__u8 *)&new_word)[0] = (__u8)((((header_bytes + ADDED_BYTES) / 4U) << 4) |
					 (((__u8 *)&old_word)[0] & 0x0fU));
	if (bpf_skb_store_bytes(skb, packet.tcp_offset + FLAGS_OFFSET, &new_word, sizeof(new_word), 0) < 0)
		return TC_ACT_SHOT;

	/* TCP checksum: the flags word, the pseudo-header length and the new bytes. */
	bpf_l4_csum_replace(skb, packet.tcp_offset + CHECKSUM_OFFSET, old_word, new_word, 2);
	bpf_l4_csum_replace(
		skb,
		packet.tcp_offset + CHECKSUM_OFFSET,
		bpf_htons(header_bytes),
		bpf_htons(header_bytes + ADDED_BYTES),
		BPF_F_PSEUDO_HDR | 2
	);
	diff = bpf_csum_diff(NULL, 0, added, sizeof(added), 0);
	bpf_l4_csum_replace(skb, packet.tcp_offset + CHECKSUM_OFFSET, 0, (__u32)diff, 0);

	if (packet.ipv6) {
		__be16 payload = bpf_htons(packet.ip_bytes - sizeof(struct ipv6hdr) + ADDED_BYTES);

		bpf_skb_store_bytes(skb, ETHERNET_BYTES + 4, &payload, sizeof(payload), 0);
	} else {
		__be16 old_length = bpf_htons(packet.ip_bytes);
		__be16 new_length = bpf_htons(packet.ip_bytes + ADDED_BYTES);

		bpf_l3_csum_replace(skb, ETHERNET_BYTES + 10, old_length, new_length, 2);
		bpf_skb_store_bytes(skb, ETHERNET_BYTES + 2, &new_length, sizeof(new_length), 0);
	}
	/* Let the stack verify the checksum in software instead of trusting the NIC's. */
	bpf_csum_level(skb, BPF_CSUM_LEVEL_RESET);
	return TC_ACT_OK;
}

SEC("tc/egress")
int strip(struct __sk_buff *skb)
{
	/* Four spare bytes so a 16-byte window from any aligned start stays inside. */
	__u8 options[TCP_OPTIONS_MAX + 4] = {};
	__u32 old_words[4];
	__u32 new_words[4];
	struct packet packet;
	struct tcphdr tcp;
	__u32 options_bytes;
	__u32 tsecr;
	__u32 start;
	__u32 span;
	__u32 first;
	__s64 diff;
	int index;

	if (parse(skb, &packet) < 0 ||
	    bpf_skb_load_bytes(skb, packet.tcp_offset, &tcp, sizeof(tcp)) < 0)
		return TC_ACT_OK;
	if (tcp.doff * 4U < TCP_HEADER_BYTES + TCP_OPTION_TIMESTAMP_LENGTH)
		return TC_ACT_OK;
	options_bytes = tcp.doff * 4U - TCP_HEADER_BYTES;
	if (options_bytes > TCP_OPTIONS_MAX)
		return TC_ACT_OK;
	if (bpf_skb_load_bytes(skb, packet.tcp_offset + TCP_HEADER_BYTES, options, options_bytes) < 0)
		return TC_ACT_OK;
	index = find_timestamp(options, options_bytes);
	if (index < 0 || index > (int)(TCP_OPTIONS_MAX - TCP_OPTION_TIMESTAMP_LENGTH))
		return TC_ACT_OK;
	__builtin_memcpy(&tsecr, &options[index + 6], sizeof(tsecr));
	if (bpf_ntohl(tsecr) != TSINJECT_MARKER)
		return TC_ACT_OK;

	/* Rewrite the 4-byte-aligned window around the option: 12 or 16 bytes. */
	start = (__u32)index & ~3U;
	if (start > TCP_OPTIONS_MAX - 12)
		return TC_ACT_OK;
	span = ((__u32)index + TCP_OPTION_TIMESTAMP_LENGTH + 3U - start) & ~3U;
	__builtin_memcpy(old_words, &options[start], sizeof(old_words));
	__builtin_memcpy(new_words, old_words, sizeof(new_words));
	first = (__u32)index - start;
	/* Constant byte indexes: the verifier rejects a variable one into this array. */
	for (__u32 k = 0; k < sizeof(new_words); k++) {
		if (k >= first && k < first + TCP_OPTION_TIMESTAMP_LENGTH)
			((__u8 *)new_words)[k] = TCP_OPTION_NOP;
	}
	/* Bytes past the span are equal in both copies and cancel out. */
	diff = bpf_csum_diff(old_words, sizeof(old_words), new_words, sizeof(new_words), 0);
	if (span == 12U) {
		if (bpf_skb_store_bytes(skb, packet.tcp_offset + TCP_HEADER_BYTES + start, new_words, 12, 0) < 0)
			return TC_ACT_OK;
	} else if (bpf_skb_store_bytes(skb, packet.tcp_offset + TCP_HEADER_BYTES + start, new_words, 16, 0) < 0) {
		return TC_ACT_OK;
	}
	bpf_l4_csum_replace(skb, packet.tcp_offset + CHECKSUM_OFFSET, 0, (__u32)diff, 0);
	return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";
