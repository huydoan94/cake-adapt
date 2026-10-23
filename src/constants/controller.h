#ifndef CONSTANTS_CONTROLLER_H_INCLUDED
#define CONSTANTS_CONTROLLER_H_INCLUDED

/* Fixed controller policy, separate from the configurable UCI values. */

#include "constants/units.h"
#include <stdint.h>

#define SATURATION_ENTER_RATIO_E6 (90U * RATIO_PERCENT_E6)
#define SATURATION_EXIT_RATIO_E6 (80U * RATIO_PERCENT_E6)
#define SATURATION_CONFIRMATION_SAMPLES 3U
#define RECOVERY_CONFIRMATION_SAMPLES 3U
/* At or above this share of the shaper rate, the bottleneck is not limiting. */
#define FULL_DELIVERY_RATIO_E6 (98U * RATIO_PERCENT_E6)

/* Measured queues smaller than this in total cannot explain bufferbloat. */
#define QUEUE_ATTRIBUTION_MINIMUM_US (5 * (int64_t)MILLISECOND)
/* A direction holding at least this share of the measured queue shares the blame. */
#define QUEUE_SHARE_RATIO_E6 (25U * RATIO_PERCENT_E6)
/* Upload kept free beyond other traffic's use, so its growth shows. */
#define UPLOAD_ACK_HEADROOM_RATIO_E6 (5U * RATIO_PERCENT_E6)

/* Room for a load condition in messages. */
#define CONTROLLER_LOAD_CONDITION_SIZE 16U

#endif
