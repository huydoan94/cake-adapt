/* Exercise raw lifetime evidence and thinning in the production socket filter. */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <bpf/bpf_helpers.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "tcpdelay/record.h"

static void *test_lookup(const void *map, const void *key);
static long test_update(const void *map, const void *key, const void *value, unsigned long flags);
static void *test_reserve(const void *map, unsigned long size, unsigned long flags);
static void test_submit(void *record, unsigned long flags);
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
#define bpf_map_update_elem test_update
#define bpf_ringbuf_reserve test_reserve
#define bpf_ringbuf_submit test_submit
#define bpf_ktime_get_ns test_now
#define bpf_skb_load_bytes test_load
#define bpf_skb_load_bytes_relative test_load_relative
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "tcpdelay/tcpdelay.bpf.c"
#pragma GCC diagnostic pop

static struct tcpdelay_record output_record;
static struct tcpdelay_record *reserved;
static struct tcpdelay_accounting accounting_model;
static struct tcpdelay_counters totals;
static struct flow_state flow_state_value;
static __u32 current_epoch = 1U;
static __u32 stream_fault_value;
static __u32 current_time = 100U;
static unsigned int output_count;
static unsigned char packet[128];
static bool reserve_failure;
static bool marker_failure;
static bool loss_marker;
static __u32 loss_epoch;
static __u32 loss_value = 1U;
static bool departure_present;
static __u32 departure_tsval;
static __u32 departure_epoch;

static void *test_lookup(const void *map, const void *key)
{
	if (map == &counters)
		return &totals;
	if (map == &accounting)
		return &accounting_model;
	if (map == &stream_epoch_v1) {
		assert(*(const __u32 *)key == 0U);
		return &current_epoch;
	}
	if (map == &stream_fault) {
		assert(*(const __u32 *)key == 0U);
		return &stream_fault_value;
	}
	if (map == &stream_losses) {
		assert(*(const __u32 *)key == current_epoch);
		return loss_marker && *(const __u32 *)key == loss_epoch ? &loss_value : NULL;
	}
	if (map == &flows)
		return flow_state_value.epoch == 0U ? NULL : &flow_state_value;
	if (map == &departures) {
		const struct departure_key *departure_key_value = key;

		return departure_present && departure_key_value->tsval == departure_tsval ?
			       &departure_epoch :
			       NULL;
	}
	assert(map == NULL && "unexpected BPF map lookup");
	return NULL;
}

static long test_update(const void *map, const void *key, const void *value, unsigned long flags)
{
	(void)key;
	(void)flags;
	if (map == &flows) {
		flow_state_value = *(const struct flow_state *)value;
		return 0;
	}
	if (map == &departures) {
		const struct departure_key *departure_key_value = key;

		departure_tsval = departure_key_value->tsval;
		departure_epoch = *(__u32 *)value;
		departure_present = true;
		return 0;
	}
	if (map == &stream_losses) {
		if (marker_failure)
			return -1;
		assert(*(__u32 *)value == 1U);
		loss_epoch = *(__u32 *)key;
		loss_marker = true;
		return 0;
	}
	assert(map == NULL && "unexpected BPF map update");
	return -1;
}

static void *test_reserve(const void *map, unsigned long size, unsigned long flags)
{
	(void)flags;
	assert(map == &samples && size == sizeof(output_record));
	if (reserve_failure)
		return NULL;
	reserved = &output_record;
	return reserved;
}

static void test_submit(void *record, unsigned long flags)
{
	assert(record == reserved && flags == BPF_RB_NO_WAKEUP);
	reserved = NULL;
	output_count++;
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
	return test_load(skb, offset + ETH_HLEN, to, length);
}

static unsigned long long test_now(void)
{
	return current_time;
}

static struct __sk_buff packet_with_tcp(struct tcphdr tcp, unsigned int ip_length)
{
	struct ethhdr ethernet = { .h_proto = bpf_htons(ETH_P_IP) };
	struct iphdr ip;

	assert(ip_length <= UINT16_MAX);
	ip = (struct iphdr){
		.version = 4U,
		.ihl = 5U,
		.protocol = IPPROTO_TCP,
		.tot_len = bpf_htons((uint16_t)ip_length),
		.saddr = bpf_htonl(0x0a000001U),
		.daddr = bpf_htonl(0x0a000002U),
	};

	memset(packet, 0, sizeof(packet));
	memcpy(packet, &ethernet, sizeof(ethernet));
	memcpy(packet + ETH_HLEN, &ip, sizeof(ip));
	memcpy(packet + ETH_HLEN + sizeof(ip), &tcp, sizeof(tcp));
	return (struct __sk_buff){
		.len = ETH_HLEN + sizeof(ip) + ip_length - sizeof(ip),
		.pkt_type = PACKET_OUTGOING,
		.protocol = bpf_htons(ETH_P_IP),
	};
}

static void test_timestamp_free_handshake_is_emitted(void)
{
	struct tcphdr tcp = { .syn = 1U,
			      .doff = 5U,
			      .source = bpf_htons(50000U),
			      .dest = bpf_htons(443U),
			      .seq = bpf_htonl(100U) };
	struct __sk_buff skb = packet_with_tcp(tcp, 40U);

	output_count = 0U;
	assert(tcpdelay(&skb) == 0);
	assert(output_count == 1U);
	assert(output_record.sequence == 100U);
	assert((output_record.metadata & TCPDELAY_STREAM_FLAG_MASK) == TCPDELAY_TCP_SYN);
	assert((output_record.metadata & TCPDELAY_STREAM_HAS_TIMESTAMP) == 0U);
	assert((output_record.metadata & TCPDELAY_STREAM_EPOCH_MASK) ==
	       (1U << TCPDELAY_STREAM_EPOCH_SHIFT));
}

static void test_first_seen_timestamp_inside_thinning_interval_stays_skipped(void)
{
	struct tcphdr tcp = { .ack = 1U,
			      .doff = 8U,
			      .source = bpf_htons(50000U),
			      .dest = bpf_htons(443U),
			      .seq = bpf_htonl(100U) };
	struct __sk_buff skb;
	uint32_t network_value;

	output_count = 0U;
	flow_state_value = (struct flow_state){
		.departure_ns = current_time,
		.outgoing_tsval = 10U,
		.epoch = current_epoch,
	};
	skb = packet_with_tcp(tcp, 52U);
	packet[ETH_HLEN + sizeof(struct iphdr) + sizeof(tcp)] = TCP_OPTION_NOP;
	packet[ETH_HLEN + sizeof(struct iphdr) + sizeof(tcp) + 1U] = TCP_OPTION_NOP;
	packet[ETH_HLEN + sizeof(struct iphdr) + sizeof(tcp) + 2U] = TCP_OPTION_TIMESTAMP;
	packet[ETH_HLEN + sizeof(struct iphdr) + sizeof(tcp) + 3U] = TCP_OPTION_TIMESTAMP_LENGTH;
	network_value = bpf_htonl(11U);
	memcpy(packet + ETH_HLEN + sizeof(struct iphdr) + sizeof(tcp) + 4U, &network_value, 4U);
	network_value = bpf_htonl(0U);
	memcpy(packet + ETH_HLEN + sizeof(struct iphdr) + sizeof(tcp) + 8U, &network_value, 4U);
	current_time += TCPDELAY_SAMPLE_INTERVAL_NS / 2U;
	assert(tcpdelay(&skb) == 0);
	assert(output_count == 0U);
	assert(flow_state_value.outgoing_tsval == 11U);
	current_time += TCPDELAY_SAMPLE_INTERVAL_NS;
	assert(tcpdelay(&skb) == 0);
	assert(output_count == 0U);
}

static void test_lifecycle_reservation_loss_poisoning(void)
{
	struct tcphdr tcp = { .syn = 1U,
			      .doff = 5U,
			      .source = bpf_htons(50000U),
			      .dest = bpf_htons(443U),
			      .seq = bpf_htonl(101U) };
	struct __sk_buff skb = packet_with_tcp(tcp, 40U);

	reserve_failure = true;
	loss_marker = false;
	marker_failure = false;
	stream_fault_value = 0U;
	totals = (struct tcpdelay_counters){ 0 };
	output_count = 0U;
	assert(tcpdelay(&skb) == 0);
	assert(output_count == 0U && totals.ring_full == 1U);
	assert(loss_marker && stream_fault_value == 0U);

	current_epoch++;
	marker_failure = true;
	assert(tcpdelay(&skb) == 0);
	assert(stream_fault_value == 1U);
	output_count = 0U;
	assert(tcpdelay(&skb) == 0);
	assert(output_count == 0U);

	current_epoch = 1U;
	stream_fault_value = 0U;
	marker_failure = false;
	reserve_failure = false;
}

static void test_matched_echo_bypasses_sampling_thinning(void)
{
	struct tcphdr tcp = { .ack = 1U,
			      .doff = 8U,
			      .source = bpf_htons(443U),
			      .dest = bpf_htons(50000U),
			      .seq = bpf_htonl(201U) };
	struct __sk_buff skb = packet_with_tcp(tcp, 52U);
	uint32_t network_value;
	struct iphdr *ip = (struct iphdr *)(void *)(packet + ETH_HLEN);

	/* Model a fresh capture after the preceding test deliberately poisoned epoch 1. */
	current_epoch = 1U;
	loss_marker = false;
	stream_fault_value = 0U;
	reserve_failure = false;
	marker_failure = false;
	ip->saddr = bpf_htonl(0x0a000002U);
	ip->daddr = bpf_htonl(0x0a000001U);
	skb.pkt_type = PACKET_HOST;
	packet[ETH_HLEN + sizeof(*ip) + sizeof(tcp)] = TCP_OPTION_NOP;
	packet[ETH_HLEN + sizeof(*ip) + sizeof(tcp) + 1U] = TCP_OPTION_NOP;
	packet[ETH_HLEN + sizeof(*ip) + sizeof(tcp) + 2U] = TCP_OPTION_TIMESTAMP;
	packet[ETH_HLEN + sizeof(*ip) + sizeof(tcp) + 3U] = TCP_OPTION_TIMESTAMP_LENGTH;
	network_value = bpf_htonl(20U);
	memcpy(packet + ETH_HLEN + sizeof(*ip) + sizeof(tcp) + 4U, &network_value, 4U);
	network_value = bpf_htonl(77U);
	memcpy(packet + ETH_HLEN + sizeof(*ip) + sizeof(tcp) + 8U, &network_value, 4U);
	flow_state_value = (struct flow_state){
		.sample_ns = current_time,
		.incoming_tsval = 19U,
		.incoming_tsecr = 76U,
		.epoch = current_epoch,
	};
	departure_present = true;
	departure_tsval = 77U;
	departure_epoch = current_epoch;
	output_count = 0U;
	current_time += TCPDELAY_SAMPLE_INTERVAL_NS / 2U;
	assert(tcpdelay(&skb) == 0);
	assert(output_count == 1U);
	assert(output_record.tsecr == 77U);
}

int main(void)
{
	accounting_model.enabled = 0U;
	test_timestamp_free_handshake_is_emitted();
	test_first_seen_timestamp_inside_thinning_interval_stays_skipped();
	test_lifecycle_reservation_loss_poisoning();
	test_matched_echo_bypasses_sampling_thinning();
	puts("filter lifetime evidence tests passed");
	return 0;
}
