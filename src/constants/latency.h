#ifndef CONSTANTS_LATENCY_H_INCLUDED
#define CONSTANTS_LATENCY_H_INCLUDED

/* Pinger methods, their command lines and output, and latency timing. */

#include "constants/units.h"
#include <stdint.h>

/* Pinger methods, as pinger_method names them. */
#define PINGER_METHOD_FPING "fping"
#define PINGER_METHOD_FPING_TS "fping-ts"
#define PINGER_METHOD_IRTT "irtt"
#define PINGER_METHOD_NATIVE "native"
#define PINGER_METHOD_NATIVE_TS "native-ts"

/* Timing. */
/* Each reply is awaited this long, as cake-autorate runs fping. */
#define LATENCY_RESPONSE_TIMEOUT_US (10U * SECOND)
#define FPING_TIMEOUT_US LATENCY_RESPONSE_TIMEOUT_US
#define LATENCY_STALE_RESPONSE_US (UINT64_C(500) * MILLISECOND)
#define LATENCY_TIMESTAMP_ROLLOVER_DELTA_US (UINT64_C(50) * MINUTE)
#define INITIAL_ONE_WAY_BASELINE_US (100U * MILLISECOND)
#define IRTT_FAST_EXIT_RETRY_US SECOND
#define IRTT_FAST_EXIT_THRESHOLD_US (UINT64_C(3) * SECOND)

/* Stopping a pinger child. */
#define CHILD_STOP_ATTEMPTS 50U
#define CHILD_STOP_INTERVAL_US (10U * MILLISECOND)
/* The event loop escalates to SIGKILL after the same bound as the shutdown stop. */
#define CHILD_STOP_TIMEOUT_US (CHILD_STOP_ATTEMPTS * CHILD_STOP_INTERVAL_US)

/* Buffers. */
/* Longest pinger output line, including its newline. */
#define LATENCY_OUTPUT_SIZE 512U
#define LATENCY_TARGET_SIZE 256U
/* Room for the pinger's own timestamp token, logged verbatim like cake-autorate. */
#define LATENCY_TIMESTAMP_TEXT_SIZE 48U
/* fping keeps six decimals: of a second for timestamps, of a millisecond for RTTs. */
#define LATENCY_FRACTION_DIGITS 6U
/* Room for one formatted number argument, such as an interval. */
#define PINGER_ARGUMENT_SIZE 32U
/* Replies read per wake-up, so foreign datagrams cannot starve the other timers. */
#define NATIVE_RECEIVE_BATCH 64U

/* fping command-line arguments and output grammar. */
#define FPING_BYTES_SEPARATOR " bytes, "
#define FPING_FIELD_SEPARATOR ", "
#define FPING_ICMP_TIMESTAMP "--icmp-timestamp"
#define FPING_INTERFACE_LONG "--iface"
#define FPING_INTERFACE_LONG_PREFIX "--iface="
#define FPING_INTERFACE_SHORT "-I"
#define FPING_INTERVAL "--interval"
#define FPING_LOCAL_RECEIVE "Localreceive="
#define FPING_LOOP "--loop"
#define FPING_MS_SUFFIX " ms"
#define FPING_ORIGINATE "Originate="
#define FPING_PATH "/usr/bin/fping"
#define FPING_PERIOD "--period"
#define FPING_RECEIVE "Receive="
#define FPING_SEQUENCE_SEPARATOR " : ["
#define FPING_TIMEOUT "--timeout"
#define FPING_TIMEOUT_SUFFIX ", timed out"
#define FPING_TIMESTAMP "--timestamp"
#define FPING_TIMESTAMPS_PREFIX " timestamps: "
#define FPING_TRANSMIT "Transmit="

/* irtt command-line arguments and output grammar. */
#define IRTT_CLIENT "client"
#define IRTT_DURATION "-d"
#define IRTT_INTERVAL "-i"
#define IRTT_PATH "/usr/bin/irtt"
#define IRTT_RECEIVE_DELAY "rd"
#define IRTT_SEND_DELAY "sd"
#define IRTT_SEQUENCE "seq"
#define IRTT_UNIT_NS "ns"
#define IRTT_UNIT_US "us"
#define IRTT_UNIT_US_SIGN "µs"
#define IRTT_UNIT_MS "ms"
#define IRTT_UNIT_SEC "s"
#define NULL_DEVICE_PATH "/dev/null"

#endif
