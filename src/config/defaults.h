#ifndef DEFAULTS_H_INCLUDED
#define DEFAULTS_H_INCLUDED

#include "common/constants.h"

struct config;

#define DEFAULT_SECTION "main"
#define DEFAULT_CONFIG_PATH "/etc/config/" UCI_PACKAGE
#define DEFAULT_LOG_DIRECTORY "/var/log"
#define DEFAULT_LOG_FILE_BASE PROGRAM_NAME
#define DEFAULT_FPING_TIMEOUT_MS "10000"

/* Fixed operational defaults, separate from configurable UCI values. */
#define CHILD_STOP_ATTEMPTS 50U
#define CHILD_STOP_INTERVAL_US (10U * MILLISECOND)
/* The event loop escalates to SIGKILL after the same bound as the shutdown stop. */
#define CHILD_STOP_TIMEOUT_US (CHILD_STOP_ATTEMPTS * CHILD_STOP_INTERVAL_US)
#define INITIAL_ONE_WAY_BASELINE_US (100U * MILLISECOND)
#define NETLINK_RESPONSE_TIMEOUT_US SECOND
#define SATURATION_ENTER_RATIO_E6 (90U * RATIO_PERCENT_E6)
#define SATURATION_EXIT_RATIO_E6 (80U * RATIO_PERCENT_E6)
#define SATURATION_CONFIRMATION_SAMPLES 3U
#define RECOVERY_CONFIRMATION_SAMPLES 3U
/* CAKE holds its rate in whole bytes/s. */
#define SHAPER_RATE_STEP_BPS BITS_PER_BYTE
/* At or above this share of the shaper rate, the bottleneck is not limiting. */
#define FULL_DELIVERY_RATIO_E6 (98U * RATIO_PERCENT_E6)
/* Measured queues smaller than this in total cannot explain bufferbloat. */
#define QUEUE_ATTRIBUTION_MINIMUM_US (5 * (int64_t)MILLISECOND)
/* A direction holding at least this share of the measured queue shares the blame. */
#define QUEUE_SHARE_RATIO_E6 (25U * RATIO_PERCENT_E6)
/* Upload kept free beyond other traffic's use, so its growth shows. */
#define UPLOAD_ACK_HEADROOM_RATIO_E6 (5U * RATIO_PERCENT_E6)
/* MEMORY records: a week is about 60,000 lines, and a leak shows within minutes. */
#define MEMORY_SAMPLE_INTERVAL_US (10U * SECOND)

/* TCP-timestamp queue estimation. */
/* Result windows; a result covers the current and the previous one. */
#define TCPDELAY_WINDOW_US (100U * MILLISECOND)
/* A remote clock needs this much history before its period is fitted. */
#define TCPDELAY_TICK_FIT_US (2U * SECOND)
/* Queues are never this long; a jump this large means the flow restarted. */
#define TCPDELAY_IMPLAUSIBLE_QUEUE_US (10U * SECOND)
/* The upload ACK rate is measured over windows of at least this length. */
#define TCPDELAY_ACK_RATE_INTERVAL_US (500U * MILLISECOND)
/* Dropped capture records are reported at most this often. */
#define TCPDELAY_COUNTER_CHECK_US MINUTE

void defaults_apply(struct config *config);

#endif
