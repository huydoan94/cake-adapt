#ifndef DEFAULTS_H_INCLUDED
#define DEFAULTS_H_INCLUDED

#include "common/constants.h"

struct config;

#define DEFAULT_SECTION "main"
#define DEFAULT_CONFIG_PATH "/etc/config/" UCI_PACKAGE
#define DEFAULT_LOG_DIRECTORY "/var/log"
#define DEFAULT_LOG_FILE_BASE PROGRAM_NAME
#define DEFAULT_FPING_TIMEOUT_MILLISECONDS "10000"

/* Fixed operational defaults, separate from configurable UCI values. */
#define CHILD_STOP_ATTEMPTS 50U
#define CHILD_STOP_INTERVAL_MILLISECONDS 10
#define CHILD_STOP_INTERVAL_NANOSECONDS \
	((long)CHILD_STOP_INTERVAL_MILLISECONDS * (long)NANOSECONDS_PER_MILLISECOND)
/* The event loop escalates to SIGKILL after the same bound as the shutdown stop. */
#define CHILD_STOP_TIMEOUT_MILLISECONDS \
	((int)CHILD_STOP_ATTEMPTS * CHILD_STOP_INTERVAL_MILLISECONDS)
#define INITIAL_ONE_WAY_BASELINE_MICROSECONDS (100U * MILLISECOND)
#define NETLINK_RESPONSE_TIMEOUT_MILLISECONDS ((int)MILLISECONDS_PER_SECOND)
#define SATURATION_ENTER_PERCENT 90U
#define SATURATION_EXIT_PERCENT 80U
#define SATURATION_CONFIRMATION_SAMPLES 3U
#define RECOVERY_CONFIRMATION_SAMPLES 3U
/* At or above this share of the shaper rate, the bottleneck is not limiting. */
#define FULL_DELIVERY_PERCENT 98U
/* Measured queues smaller than this in total cannot explain bufferbloat. */
#define QUEUE_ATTRIBUTION_MINIMUM_MICROSECONDS (5 * (int64_t)MILLISECOND)
/* A direction holding at least 1/4 of the measured queue shares the blame. */
#define QUEUE_SHARE_DIVISOR 4
/* Upload kept free beyond other traffic's use, so its growth shows (percent). */
#define UPLOAD_ACK_HEADROOM_PERCENT 5U

/* TCP-timestamp queue estimation. */
/* Result windows; a result covers the current and the previous one. */
#define TCPDELAY_WINDOW_MICROSECONDS (100U * MILLISECOND)
/* FOLLOW baselines use two such buckets; HOLD retains their lowest minimum. */
#define TCPDELAY_FLOOR_BUCKET_MICROSECONDS (30U * SECOND)
/* A remote clock needs this much history before its period is fitted. */
#define TCPDELAY_TICK_FIT_MICROSECONDS (2U * SECOND)
/* Queues are never this long; a jump this large means the flow restarted. */
#define TCPDELAY_IMPLAUSIBLE_QUEUE_MICROSECONDS (10U * SECOND)
/* The upload ACK rate is measured over windows of at least this length. */
#define TCPDELAY_ACK_RATE_INTERVAL_MICROSECONDS (500U * MILLISECOND)
/* Dropped capture records are reported at most this often. */
#define TCPDELAY_COUNTER_CHECK_MICROSECONDS MINUTE

void defaults_apply(struct config *config);

#endif
