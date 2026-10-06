/* Exercise the production filter with native packet/map helper substitutes. */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <bpf/bpf_helpers.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void *test_lookup(const void *map, const void *key);
static long
test_load(const struct __sk_buff *skb, unsigned int offset, void *to, unsigned int length);
static long test_load_relative(
	const struct __sk_buff *skb,
	unsigned int offset,
	void *to,
	unsigned int length,
	unsigned int start
);
static unsigned long long test_now(void);

#define bpf_map_lookup_elem test_lookup
#define bpf_skb_load_bytes test_load
#define bpf_skb_load_bytes_relative test_load_relative
#define bpf_ktime_get_ns test_now
/* The BPF source uses GNU empty initializers and helpers returning long; its
 * existing native sizeof/int conversions are outside the tested substitutions.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "tcpdelay/tcpdelay.bpf.c"
#pragma GCC diagnostic pop

static struct tcpdelay_accounting model;
static struct tcpdelay_counters totals;
static unsigned char packet[128];

static unsigned long long test_now(void)
{
	return 1U;
}

static void *test_lookup(const void *map, const void *key)
{
	assert(*(const __u32 *)key == 0U);
	if (map == &counters)
		return &totals;
	assert(map == &accounting);
	return &model;
}

static long
test_load(const struct __sk_buff *skb, unsigned int offset, void *to, unsigned int length)
{
	if (offset > skb->len || length > skb->len - offset)
		return -1;
	memcpy(to, packet + offset, length);
	return 0;
}

static long test_load_relative(
	const struct __sk_buff *skb,
	unsigned int offset,
	void *to,
	unsigned int length,
	unsigned int start
)
{
	assert(start == BPF_HDR_START_NET);
	unsigned int network = model.hardware_type == ARPHRD_PPP ||
					       model.hardware_type == ARPHRD_NONE ||
					       model.hardware_type == ARPHRD_RAWIP ?
				       0U :
				       ETH_HLEN;
	return test_load(skb, offset + network, to, length);
}

static struct __sk_buff ack_packet(void)
{
	struct ethhdr ethernet = { .h_proto = bpf_htons(ETH_P_IP) };
	struct iphdr ip = { .version = 4U,
			    .ihl = 5U,
			    .protocol = IPPROTO_TCP,
			    .tot_len = bpf_htons(40U) };
	struct tcphdr tcp = { .ack = 1U, .doff = 5U };
	memset(packet, 0, sizeof(packet));
	memcpy(packet, &ethernet, sizeof(ethernet));
	memcpy(packet + ETH_HLEN, &ip, sizeof(ip));
	memcpy(packet + ETH_HLEN + sizeof(ip), &tcp, sizeof(tcp));
	totals = (struct tcpdelay_counters){ 0 };
	model = (struct tcpdelay_accounting){ .cake = { .atm_mode = CAKE_ATM_NONE },
					      .hardware_type = ARPHRD_ETHER,
					      .enabled = 1U };
	return (struct __sk_buff){ .len = 54U,
				   .pkt_type = PACKET_OUTGOING,
				   .protocol = bpf_htons(ETH_P_IP) };
}

static void rejected(struct __sk_buff *skb)
{
	assert(tcpdelay(skb) == 0);
	assert(totals.ack_bytes == 0U && totals.upload_bytes == 0U);
	assert(totals.unaccounted_packets == 1U);
}

int main(void)
{
	struct __sk_buff skb = ack_packet();
	model.cake.overhead_bytes = 44;
	assert(tcpdelay(&skb) == 0);
	assert(totals.ack_bytes == 84U && totals.upload_bytes == 84U);
	assert(totals.unaccounted_packets == 0U);
	skb = ack_packet();
	skb.gso_size = 1000U;
	rejected(&skb);
	skb = ack_packet();
	skb.gso_size = 1000U;
	model.cake.raw = 1U;
	rejected(&skb);
	skb = ack_packet();
	skb.vlan_present = 1U;
	rejected(&skb);
	skb = ack_packet();
	packet[12] = 0x81U;
	packet[13] = 0U;
	rejected(&skb);
	skb = ack_packet();
	packet[12] = 0x88U;
	packet[13] = 0xa8U;
	rejected(&skb);
	skb = ack_packet();
	packet[12] = 0x88U;
	packet[13] = 0xb5U;
	rejected(&skb);
	skb = ack_packet();
	model.hardware_type = ARPHRD_LOOPBACK;
	rejected(&skb);
	skb = ack_packet();
	model.cake.atm_mode = 255U;
	rejected(&skb);
	skb = ack_packet();
	model.enabled = 0U;
	skb.gso_size = 1000U;
	assert(tcpdelay(&skb) == 0);
	assert(totals.ack_bytes == 54U && totals.upload_bytes == 54U);
	assert(totals.unaccounted_packets == 0U);
	skb = ack_packet();
	model.cake.raw = 1U;
	skb.protocol = bpf_htons(0x88b5U);
	packet[12] = 0x88U;
	packet[13] = 0xb5U;
	assert(tcpdelay(&skb) == 0);
	assert(totals.ack_bytes == 0U && totals.upload_bytes == 54U);
	assert(totals.unaccounted_packets == 0U);
	/* Positive link/protocol coverage uses the same actual filter entry. */
	for (unsigned int link = 0U; link < 3U; link++) {
		const unsigned int types[] = { ARPHRD_PPP, ARPHRD_NONE, ARPHRD_RAWIP };
		skb = ack_packet();
		model.hardware_type = types[link];
		memmove(packet, packet + ETH_HLEN, 40U);
		skb.len = 40U;
		assert(tcpdelay(&skb) == 0);
		assert(totals.ack_bytes == 40U && totals.upload_bytes == 40U);
		assert(totals.unaccounted_packets == 0U);
	}
	skb = ack_packet();
	skb.protocol = bpf_htons(ETH_P_ARP);
	packet[12] = 0x08U;
	packet[13] = 0x06U;
	skb.len = 42U;
	assert(tcpdelay(&skb) == 0);
	assert(totals.ack_bytes == 0U && totals.upload_bytes == 28U);
	assert(totals.unaccounted_packets == 0U);
	{
		struct ethhdr ethernet = { .h_proto = bpf_htons(ETH_P_IPV6) };
		struct ipv6hdr ip6 = { .version = 6U,
				       .nexthdr = IPPROTO_TCP,
				       .payload_len = bpf_htons(20U) };
		struct tcphdr tcp = { .ack = 1U, .doff = 5U };
		skb = ack_packet();
		memcpy(packet, &ethernet, sizeof(ethernet));
		memcpy(packet + ETH_HLEN, &ip6, sizeof(ip6));
		memcpy(packet + ETH_HLEN + sizeof(ip6), &tcp, sizeof(tcp));
		skb.len = 74U;
		skb.protocol = bpf_htons(ETH_P_IPV6);
		assert(tcpdelay(&skb) == 0);
		assert(totals.ack_bytes == 60U && totals.upload_bytes == 60U);
		assert(totals.unaccounted_packets == 0U);
	}
	puts("filter accounting fallback tests passed");
	return 0;
}
