#ifndef CONSTANTS_TCPDELAY_H_INCLUDED
#define CONSTANTS_TCPDELAY_H_INCLUDED

/* TCP-timestamp queue measurement: the filter object, maps, packet layout and estimator. */

#include "constants/units.h"
#include <stdint.h>

#define TCPDELAY_OBJECT_PATH "/lib/bpf/cake-adapt-tcpdelay.o"

/* Names in tcpdelay.bpf.c: the programs and their maps. */
#define TCPDELAY_FILTER_PROGRAM "tcpdelay"
#define TCPDELAY_FILTER_COUNTERS "counters"
#define TCPDELAY_FILTER_ACCOUNTING "accounting"
#define TCPDELAY_FILTER_SAMPLES "samples"
#define TCPDELAY_INJECT_PROGRAM "inject_egress"
#define TCPDELAY_INJECT_SERVERS_MAP "inject_servers"
#define TCPDELAY_INJECT_HANDSHAKES_MAP "inject_handshakes"
#define TCPDELAY_INJECT_CLIENTS_MAP "inject_clients"
#define TCPDELAY_INJECT_COUNTERS_MAP "inject_counters"
#define TCPDELAY_INJECT_SETTINGS_MAP "inject_settings"

/* The filter: map sizes and sampling. */
#define TCPDELAY_DEPARTURES 8192
#define TCPDELAY_FLOW_STATES 1024
#define TCPDELAY_RING_BYTES (256U * KILOBYTE)
/*
 * Per flow, at most one departure is recorded per interval, and at most one
 * reply that echoes no recorded departure is sampled per interval.
 */
#define TCPDELAY_SAMPLE_INTERVAL_NS (4U * NANOSECONDS_PER_MILLISECOND)

/* TCP and IPv6 header layout, as the filter and injector parse it. */
#define TCP_HEADER_BYTES 20U
#define TCP_HEADER_MAX_BYTES 60U
#define TCP_FLAGS_OFFSET 12U
#define TCP_CHECKSUM_OFFSET 16U
#define TCP_OPTIONS_MAX 40U
#define TCP_OPTION_END 0U
#define TCP_OPTION_NOP 1U
#define TCP_OPTION_TIMESTAMP 8U
#define TCP_OPTION_TIMESTAMP_LENGTH 10U
#define IPV6_LENGTH_OFFSET 4U

/* Timestamp injection. */
/*
 * The TSval of an injected SYN while no better one is known; a SYN-ACK of the
 * same 4-tuple that echoes the injected value accepted the timestamp. Not zero,
 * which some stacks read as no echo.
 */
#define TCPDELAY_INJECT_TSVAL 1U
/*
 * For a client whose handshake stalled with TSVAL, until its clock is
 * learned: half the clock away, so it suits the clocks TSVAL does not (Windows
 * up 24.8 to 49.7 days).
 */
#define TCPDELAY_INJECT_STALLED_TSVAL 0x80000000U
/* Each map holds about 100 KB of kernel memory. */
#define TCPDELAY_INJECT_SERVERS 1024
#define TCPDELAY_INJECT_HANDSHAKES 1024
#define TCPDELAY_INJECT_CLIENTS 1024
/* A skip and a learned clock each last a day. */
#define TCPDELAY_INJECT_DAY_NS (24U * 60U * MINUTE * NANOSECONDS_PER_MICROSECOND)
/* The injection maps' size while injection is off; the filter still looks them up. */
#define TCPDELAY_INJECT_UNUSED_ENTRIES 1U
/* NOP, NOP and a timestamp option, appended to a SYN. */
#define TCPDELAY_INJECT_ADDED_BYTES 12U

/* The estimator, in microseconds as configured and in the samples' kernel nanoseconds. */
/* Result windows; a result covers the current and the previous one. */
#define TCPDELAY_WINDOW_US (100U * MILLISECOND)
/* A remote clock needs this much history before its period is fitted. */
#define TCPDELAY_TICK_FIT_US (2U * SECOND)
/* Queues are never this long; a jump this large means the flow restarted. */
#define TCPDELAY_IMPLAUSIBLE_QUEUE_US (10U * SECOND)
#define TCPDELAY_WINDOW_NS (TCPDELAY_WINDOW_US * NANOSECONDS_PER_MICROSECOND)
#define TCPDELAY_TICK_SPAN_NS (TCPDELAY_TICK_FIT_US * NANOSECONDS_PER_MICROSECOND)
#define TCPDELAY_IMPLAUSIBLE_QUEUE_NS \
	((int64_t)(TCPDELAY_IMPLAUSIBLE_QUEUE_US * NANOSECONDS_PER_MICROSECOND))
/* A fitted period may differ this much from the standard one it snaps to. */
#define TCPDELAY_TICK_TOLERANCE_RATIO_E6 (5U * RATIO_PERCENT_E6)
/* Flow slots; the flow with the least recently accepted sample is replaced. */
#define TCPDELAY_FLOWS 32U
/* Standard TCP timestamp clock periods: 1, 4, 10 and 100 ms. */
#define TCPDELAY_TICKS 4U

/* Upload ACK rates and dropped-record reports. */
/* The upload ACK rate is measured over windows of at least this length. */
#define TCPDELAY_ACK_RATE_INTERVAL_US (500U * MILLISECOND)
/* Dropped capture records are reported at most this often. */
#define TCPDELAY_COUNTER_CHECK_US MINUTE

#endif
