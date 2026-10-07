#include "latency/tracker.h"
#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "config/defaults.h"

#include <stdbool.h>
#include <stdint.h>

void tracker_init(struct latency_tracker *tracker, const struct latency_tracker_config *config)
{
	tracker->config = *config;
	tracker_reset(tracker);
}

void tracker_reset(struct latency_tracker *tracker)
{
	tracker->download.baseline_us = (int64_t)INITIAL_ONE_WAY_BASELINE_US;
	tracker->download.delta_ewma_us = 0;
	tracker->upload = tracker->download;
}

static bool sample_has_timestamp_rollover(
	const struct latency_tracker *tracker,
	const struct latency_sample *sample
)
{
	uint64_t download_delta;
	uint64_t upload_delta;

	if (!sample->timestamp_rollover_sensitive)
		return false;
	download_delta =
		absolute_difference(sample->download_owd_us, tracker->download.baseline_us);
	upload_delta = absolute_difference(sample->upload_owd_us, tracker->upload.baseline_us);
	return download_delta >= LATENCY_TIMESTAMP_ROLLOVER_DELTA_US ||
	       upload_delta >= LATENCY_TIMESTAMP_ROLLOVER_DELTA_US - download_delta;
}

/*
 * (alpha * value + (RATIO_ONE_E6 - alpha) * average) / RATIO_ONE_E6 in whole
 * microseconds, half a microsecond or more rounding away from zero. A sum that does not
 * fit in 64 bits cannot be averaged.
 */
static bool
weighted_average(int64_t alpha, int64_t value_us, int64_t average_us, int64_t *result_us)
{
	int64_t value_component;
	int64_t average_component;
	int64_t weighted;

	if (__builtin_mul_overflow(alpha, value_us, &value_component) ||
	    __builtin_mul_overflow((int64_t)RATIO_ONE_E6 - alpha, average_us, &average_component) ||
	    __builtin_add_overflow(value_component, average_component, &weighted)) {
		return false;
	}
	*result_us = signed_rounded_divide(weighted, (int64_t)RATIO_ONE_E6);
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
	int64_t alpha = (int64_t)(value_us >= state->baseline_us ?
					  config->alpha_baseline_increase_ratio_e6 :
					  config->alpha_baseline_decrease_ratio_e6);

	if (!weighted_average(alpha, value_us, state->baseline_us, &state->baseline_us)) {
		/* Extreme timestamp values cannot preserve a meaningful EWMA. */
		state->baseline_us = value_us;
	}

	return signed_difference(value_us, state->baseline_us);
}

static void
report_delta_ewma(const struct latency_tracker *tracker, struct latency_observation *observation)
{
	observation->download_owd_delta_ewma_us = tracker->download.delta_ewma_us;
	observation->upload_owd_delta_ewma_us = tracker->upload.delta_ewma_us;
}

void tracker_update(
	struct latency_tracker *tracker,
	const struct latency_sample *sample,
	struct latency_observation *observation
)
{
	observation->download_owd_us = sample->download_owd_us;
	observation->upload_owd_us = sample->upload_owd_us;
	if (sample_has_timestamp_rollover(tracker, sample)) {
		/* Restart both baselines at the sample, so neither direction has a delta. */
		tracker->download.baseline_us = sample->download_owd_us;
		tracker->upload.baseline_us = sample->upload_owd_us;
		observation->download_owd_baseline_us = sample->download_owd_us;
		observation->download_owd_delta_us = 0;
		observation->upload_owd_baseline_us = sample->upload_owd_us;
		observation->upload_owd_delta_us = 0;
	} else {
		observation->download_owd_delta_us = tracker_update_direction(
			tracker,
			&tracker->download,
			sample->download_owd_us
		);
		observation->download_owd_baseline_us = tracker->download.baseline_us;
		observation->upload_owd_delta_us =
			tracker_update_direction(tracker, &tracker->upload, sample->upload_owd_us);
		observation->upload_owd_baseline_us = tracker->upload.baseline_us;
	}
	report_delta_ewma(tracker, observation);
}

static void update_delta_ewma(int64_t alpha, int64_t delta_us, int64_t *ewma_us)
{
	/* Like the baseline, an EWMA that cannot be represented restarts at the sample. */
	if (!weighted_average(alpha, delta_us, *ewma_us, ewma_us))
		*ewma_us = delta_us;
}

void tracker_update_delta_ewma(
	struct latency_tracker *tracker,
	bool low_load,
	struct latency_observation *observation
)
{
	const struct latency_tracker_config *config = &tracker->config;
	int64_t alpha = (int64_t)config->alpha_delta_ewma_ratio_e6;

	/* cake-autorate freezes reflector delay EWMA while either link is busy. */
	if (low_load) {
		update_delta_ewma(
			alpha,
			observation->download_owd_delta_us,
			&tracker->download.delta_ewma_us
		);
		update_delta_ewma(
			alpha,
			observation->upload_owd_delta_us,
			&tracker->upload.delta_ewma_us
		);
	}
	report_delta_ewma(tracker, observation);
}
