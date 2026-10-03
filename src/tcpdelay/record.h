#ifndef TCPDELAY_RECORD_H_INCLUDED
#define TCPDELAY_RECORD_H_INCLUDED

/* Shared by tcpdelay.bpf.c and capture.c; kernel types work on both sides. */
#include <linux/types.h>

#define TCPDELAY_DEPARTURES 8192
#define TCPDELAY_FLOW_STATES 1024
#define TCPDELAY_RING_BYTES (256 * 1024)

#define TCPDELAY_COUNTER_OUTGOING 0
#define TCPDELAY_COUNTER_INCOMING 1
#define TCPDELAY_COUNTER_MATCHED 2
#define TCPDELAY_COUNTER_RING_FULL 3
/* Bytes of outgoing pure ACKs, and of all outgoing packets; link-layer header
 * included, as CAKE counts them. */
#define TCPDELAY_COUNTER_ACK_BYTES 4
#define TCPDELAY_COUNTER_UPLOAD_BYTES 5
#define TCPDELAY_COUNTERS 6

/* Addresses are IPv4-mapped IPv6; ports are in network byte order. */
struct tcpdelay_record_flow {
    __u8 local_address[16];
    __u8 remote_address[16];
    __u16 local_port;
    __u16 remote_port;
};

/*
 * Times are CLOCK_MONOTONIC nanoseconds. The 64-bit fields come first and
 * the size is a multiple of 8, so 32-bit userspace, which aligns 64-bit
 * fields to 4 bytes, sees the same layout as the 64-bit BPF program.
 */
struct tcpdelay_record {
    __u64 arrival_ns;
    /* Departure of the echoed TSecr; zero when it was not seen leaving. */
    __u64 departure_ns;
    struct tcpdelay_record_flow flow;
    __u32 tsval;
    __u32 tsecr;
    __u32 reserved;
};

_Static_assert(sizeof(struct tcpdelay_record) == 64, "record layout must match on both sides");

#endif
