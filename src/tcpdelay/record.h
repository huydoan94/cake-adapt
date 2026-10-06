#ifndef TCPDELAY_RECORD_H_INCLUDED
#define TCPDELAY_RECORD_H_INCLUDED

/* Shared by tcpdelay.bpf.c and capture.c; kernel types work on both sides. */
#include <linux/types.h>

#include "cake/accounting.h"

#define TCPDELAY_DEPARTURES 8192
#define TCPDELAY_FLOW_STATES 1024
#define TCPDELAY_RING_BYTES (256 * 1024)
#define TCPDELAY_STREAM_EPOCHS 64U
#define TCPDELAY_STREAM_LOSSES 64U
#define TCPDELAY_STREAM_VERSION 1U
#define TCPDELAY_STREAM_EPOCH_SHIFT 16U
#define TCPDELAY_STREAM_VERSION_SHIFT 10U
#define TCPDELAY_STREAM_FLAG_MASK 0xffU
#define TCPDELAY_STREAM_OUTGOING (1U << 8)
#define TCPDELAY_STREAM_HAS_TIMESTAMP (1U << 9)
#define TCPDELAY_STREAM_VERSION_MASK (0x3fU << TCPDELAY_STREAM_VERSION_SHIFT)
#define TCPDELAY_STREAM_EPOCH_MASK (0xffffU << TCPDELAY_STREAM_EPOCH_SHIFT)
#define TCPDELAY_TCP_SYN 0x02U
#define TCPDELAY_TCP_FIN 0x01U
#define TCPDELAY_TCP_RST 0x04U
#define TCPDELAY_TCP_PSH 0x08U
#define TCPDELAY_TCP_ACK 0x10U
#define TCPDELAY_TCP_URG 0x20U
#define TCPDELAY_TCP_ECE 0x40U
#define TCPDELAY_TCP_CWR 0x80U
#define TCPDELAY_LIFETIME_SLOTS 1024U
#define TCPDELAY_DEPARTURE_SLOTS 8192U
#define TCPDELAY_LIFETIME_HISTORY 8U
#define TCPDELAY_STREAM_MAX_DRAIN 4096U
/*
 * Per flow, at most one departure is recorded per interval, and at most one
 * reply that echoes no recorded departure is sampled per interval.
 */
#define TCPDELAY_SAMPLE_INTERVAL_NS (4ULL * 1000ULL * 1000ULL)

/* Addresses are IPv4-mapped IPv6; ports are in network byte order. */
struct tcpdelay_record_flow {
	__u8 local_address[16];
	__u8 remote_address[16];
	__u16 local_port;
	__u16 remote_port;
};

/*
 * The filter's counters, one copy per CPU in a single map entry, so a packet
 * and a userspace read each need one lookup.
 */
struct tcpdelay_counters {
	__u64 ring_full;
	/* CAKE charges when accounting is enabled; otherwise raw observed bytes. */
	__u64 ack_bytes;
	__u64 upload_bytes;
	/* Unsupported accounting invalidates the ACK-rate interval, not TCP timing. */
	__u64 unaccounted_packets;
};

/* Written before socket bind, then immutable for this capture's lifetime. */
struct tcpdelay_accounting {
	struct cake_accounting cake;
	__u32 hardware_type;
	__u32 enabled;
};

_Static_assert(sizeof(struct tcpdelay_accounting) == 24, "accounting map layout");
_Static_assert(sizeof(struct tcpdelay_counters) == 32, "counter map layout");

/* Times are CLOCK_MONOTONIC nanoseconds; explicit offsets define stream ABI v1. */
struct tcpdelay_record {
	__u64 arrival_ns;
	struct tcpdelay_record_flow flow;
	__u32 tsval;
	__u32 tsecr;
	__u32 sequence;
	__u32 acknowledgment;
	__u32 metadata;
};

_Static_assert(sizeof(struct tcpdelay_record) == 64, "record layout must match on both sides");
_Static_assert(__builtin_offsetof(struct tcpdelay_record, arrival_ns) == 0, "record time offset");
_Static_assert(__builtin_offsetof(struct tcpdelay_record, flow) == 8, "record flow offset");
_Static_assert(__builtin_offsetof(struct tcpdelay_record, tsval) == 44, "record TSval offset");
_Static_assert(__builtin_offsetof(struct tcpdelay_record, tsecr) == 48, "record TSecr offset");
_Static_assert(__builtin_offsetof(struct tcpdelay_record, sequence) == 52, "record sequence offset");
_Static_assert(
	__builtin_offsetof(struct tcpdelay_record, acknowledgment) == 56,
	"record acknowledgment offset"
);
_Static_assert(__builtin_offsetof(struct tcpdelay_record, metadata) == 60, "record metadata offset");

#endif
