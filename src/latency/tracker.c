#include "latency/tracker.h"
#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "config/defaults.h"

#include <stdbool.h>
#include <stdint.h>

void tracker_init(struct latency_tracker *tracker, const struct latency_tracker_config *config)
{
	tracker->config = config;
	tracker_reset(tracker);
}

void tracker_reset(struct latency_tracker *tracker)
{
	tracker->download.baseline_microseconds = (int64_t)INITIAL_ONE_WAY_BASELINE_MICROSECONDS;
	tracker->download.delta_ewma_microseconds = 0;
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
	download_delta = absolute_difference(
		sample->download_owd_microseconds,
		tracker->download.baseline_microseconds
	);
	upload_delta = absolute_difference(
		sample->upload_owd_microseconds,
		tracker->upload.baseline_microseconds
	);
	return download_delta >= LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS ||
	       upload_delta >= LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS - download_delta;
}

static bool weighted_average(
	int64_t alpha,
	int64_t value_microseconds,
	int64_t baseline_microseconds,
	int64_t *average_microseconds
)
{
	int64_t value_component;
	int64_t baseline_component;
	int64_t weighted;

	if (__builtin_mul_overflow(alpha, value_microseconds, &value_component) ||
	    __builtin_mul_overflow(
		    (int64_t)MILLION - alpha,
		    baseline_microseconds,
		    &baseline_component
	    ) ||
	    __builtin_add_overflow(value_component, baseline_component, &weighted)) {
		return false;
	}
	*average_microseconds = weighted / (int64_t)MILLION;
	return true;
}

/* Moves the baseline toward the sample and returns the sample's delta from it. */
static int64_t tracker_update_direction(
	const struct latency_tracker_config *config,
	struct latency_direction_tracker *state,
	int64_t value_microseconds
)
{
	int64_t alpha = value_microseconds >= state->baseline_microseconds ?
				(int64_t)config->alpha_baseline_increase_per_million :
				(int64_t)config->alpha_baseline_decrease_per_million;

	if (!weighted_average(
		    alpha,
		    value_microseconds,
		    state->baseline_microseconds,
		    &state->baseline_microseconds
	    )) {
		/* Extreme timestamp values cannot preserve a meaningful EWMA. */
		state->baseline_microseconds = value_microseconds;
	}

	return signed_difference(value_microseconds, state->baseline_microseconds);
}

static void
report_delta_ewma(const struct latency_tracker *tracker, struct latency_observation *observation)
{
	observation->download_owd_delta_ewma_microseconds =
		tracker->download.delta_ewma_microseconds;
	observation->upload_owd_delta_ewma_microseconds = tracker->upload.delta_ewma_microseconds;
}

void tracker_update(
	struct latency_tracker *tracker,
	const struct latency_sample *sample,
	struct latency_observation *observation
)
{
	observation->download_owd_microseconds = sample->download_owd_microseconds;
	observation->upload_owd_microseconds = sample->upload_owd_microseconds;
	if (sample_has_timestamp_rollover(tracker, sample)) {
		/* Restart both baselines at the sample, so neither direction has a delta. */
		tracker->download.baseline_microseconds = sample->download_owd_microseconds;
		tracker->upload.baseline_microseconds = sample->upload_owd_microseconds;
		observation->download_owd_baseline_microseconds = sample->download_owd_microseconds;
		observation->download_owd_delta_microseconds = 0;
		observation->upload_owd_baseline_microseconds = sample->upload_owd_microseconds;
		observation->upload_owd_delta_microseconds = 0;
	} else {
		observation->download_owd_delta_microseconds = tracker_update_direction(
			tracker->config,
			&tracker->download,
			sample->download_owd_microseconds
		);
		observation->download_owd_baseline_microseconds =
			tracker->download.baseline_microseconds;
		observation->upload_owd_delta_microseconds = tracker_update_direction(
			tracker->config,
			&tracker->upload,
			sample->upload_owd_microseconds
		);
		observation->upload_owd_baseline_microseconds =
			tracker->upload.baseline_microseconds;
	}
	report_delta_ewma(tracker, observation);
}

static void update_delta_ewma(int64_t alpha, int64_t delta_microseconds, int64_t *ewma_microseconds)
{
	/* Like the baseline, an EWMA that cannot be represented restarts at the sample. */
	if (!weighted_average(alpha, delta_microseconds, *ewma_microseconds, ewma_microseconds))
		*ewma_microseconds = delta_microseconds;
}

void tracker_update_delta_ewma(
	struct latency_tracker *tracker,
	bool low_load,
	struct latency_observation *observation
)
{
	int64_t alpha = (int64_t)tracker->config->alpha_delta_ewma_per_million;

	/* cake-autorate freezes reflector delay EWMA while either link is busy. */
	if (low_load) {
		update_delta_ewma(
			alpha,
			observation->download_owd_delta_microseconds,
			&tracker->download.delta_ewma_microseconds
		);
		update_delta_ewma(
			alpha,
			observation->upload_owd_delta_microseconds,
			&tracker->upload.delta_ewma_microseconds
		);
	}
	report_delta_ewma(tracker, observation);
}
