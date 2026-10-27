#include "latency/tracker.h"
#include "common/arith.h"
#include "config/defaults.h"
#include "constants/latency.h"
#include "constants/units.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void tracker_init(struct latency_tracker *tracker, const struct latency_tracker_config *config)
{
	tracker->config = *config;
	tracker_reset(tracker);
}

void tracker_reset(struct latency_tracker *tracker)
{
	const struct latency_direction_tracker initial = {
		.baseline_us = (int64_t)INITIAL_ONE_WAY_BASELINE_US,
	};

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++)
		tracker->directions[direction] = initial;
}

static bool sample_has_timestamp_rollover(
	const struct latency_tracker *tracker,
	const struct latency_sample *sample
)
{
	const struct latency_direction_tracker *download = &tracker->directions[DIRECTION_DOWNLOAD];
	const struct latency_direction_tracker *upload = &tracker->directions[DIRECTION_UPLOAD];
	uint64_t download_delta_us;
	uint64_t upload_delta_us;

	if (!sample->timestamp_rollover_sensitive)
		return false;
	download_delta_us =
		absolute_difference(sample->owd_us[DIRECTION_DOWNLOAD], download->baseline_us);
	upload_delta_us =
		absolute_difference(sample->owd_us[DIRECTION_UPLOAD], upload->baseline_us);
	return download_delta_us >= LATENCY_TIMESTAMP_ROLLOVER_DELTA_US ||
	       upload_delta_us >= LATENCY_TIMESTAMP_ROLLOVER_DELTA_US - download_delta_us;
}

/*
 * (alpha * value + (RATIO_ONE_E6 - alpha) * average) / RATIO_ONE_E6 in whole
 * microseconds, half a microsecond or more rounding away from zero. A sum that does not
 * fit in 64 bits cannot be averaged.
 */
static bool
weighted_average(int64_t alpha_e6, int64_t value_us, int64_t average_us, int64_t *result_us)
{
	int64_t value_component_us_e6;
	int64_t average_component_us_e6;
	int64_t weighted_us_e6;

	if (__builtin_mul_overflow(alpha_e6, value_us, &value_component_us_e6) ||
	    __builtin_mul_overflow(
		    (int64_t)RATIO_ONE_E6 - alpha_e6,
		    average_us,
		    &average_component_us_e6
	    ) ||
	    __builtin_add_overflow(value_component_us_e6, average_component_us_e6, &weighted_us_e6)) {
		return false;
	}
	*result_us = signed_rounded_divide(weighted_us_e6, (int64_t)RATIO_ONE_E6);
	return true;
}

/* Moves the baseline toward the sample and returns the sample's delta from it. */
static int64_t tracker_update_direction(
	const struct latency_tracker *tracker,
	struct latency_direction_tracker *state,
	int64_t value_us
)
{
	const struct latency_tracker_config *config = &tracker->config;
	int64_t alpha_e6 = (int64_t)(value_us >= state->baseline_us ?
					     config->alpha_baseline_increase_ratio_e6 :
					     config->alpha_baseline_decrease_ratio_e6);

	if (!weighted_average(alpha_e6, value_us, state->baseline_us, &state->baseline_us)) {
		/* Extreme timestamp values cannot preserve a meaningful EWMA. */
		state->baseline_us = value_us;
	}

	return signed_difference(value_us, state->baseline_us);
}

void tracker_update(
	struct latency_tracker *tracker,
	const struct latency_sample *sample,
	struct latency_observation *observation
)
{
	/* Restart both baselines at a rolled-over sample, so neither direction has a delta. */
	bool rollover = sample_has_timestamp_rollover(tracker, sample);

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
		struct latency_direction_tracker *state = &tracker->directions[direction];
		int64_t owd_us = sample->owd_us[direction];

		observation->owd_us[direction] = owd_us;
		if (rollover) {
			state->baseline_us = owd_us;
			observation->owd_delta_us[direction] = 0;
		} else {
			observation->owd_delta_us[direction] =
				tracker_update_direction(tracker, state, owd_us);
		}
		observation->owd_baseline_us[direction] = state->baseline_us;
		observation->owd_delta_ewma_us[direction] = state->delta_ewma_us;
	}
}

static void update_delta_ewma(int64_t alpha_e6, int64_t delta_us, int64_t *ewma_us)
{
	/* Like the baseline, an EWMA that cannot be represented restarts at the sample. */
	if (!weighted_average(alpha_e6, delta_us, *ewma_us, ewma_us))
		*ewma_us = delta_us;
}

void tracker_update_delta_ewma(
	struct latency_tracker *tracker,
	bool low_load,
	struct latency_observation *observation
)
{
	const struct latency_tracker_config *config = &tracker->config;
	int64_t alpha_e6 = (int64_t)config->alpha_delta_ewma_ratio_e6;

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
		struct latency_direction_tracker *state = &tracker->directions[direction];

		/* cake-autorate freezes reflector delay EWMA while either link is busy. */
		if (low_load)
			update_delta_ewma(
				alpha_e6,
				observation->owd_delta_us[direction],
				&state->delta_ewma_us
			);
		observation->owd_delta_ewma_us[direction] = state->delta_ewma_us;
	}
}
