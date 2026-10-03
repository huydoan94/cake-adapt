#include "latency/tracker.h"
#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "config/defaults.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

int tracker_init(struct latency_tracker *tracker, const struct latency_tracker_config *config)
{
	if (config->alpha_baseline_increase_per_million > MILLION ||
	    config->alpha_baseline_decrease_per_million > MILLION ||
	    config->alpha_delta_ewma_per_million > MILLION) {
		errno = EINVAL;
		return -1;
	}

	tracker->config = *config;
	tracker_reset(tracker);
	return 0;
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

static void tracker_update_direction(
	const struct latency_tracker_config *config,
	struct latency_direction_tracker *state,
	int64_t value_microseconds,
	int64_t *baseline_microseconds,
	int64_t *delta_microseconds
)
{
	int64_t alpha = value_microseconds >= state->baseline_microseconds
				? (int64_t)config->alpha_baseline_increase_per_million
				: (int64_t)config->alpha_baseline_decrease_per_million;

	if (!weighted_average(
		    alpha,
		    value_microseconds,
		    state->baseline_microseconds,
		    &state->baseline_microseconds
	    )) {
		/* Extreme timestamp values cannot preserve a meaningful EWMA. */
		state->baseline_microseconds = value_microseconds;
	}

	*baseline_microseconds = state->baseline_microseconds;
	*delta_microseconds = signed_difference(value_microseconds, state->baseline_microseconds);
}

void tracker_update(
	struct latency_tracker *tracker,
	const struct latency_sample *sample,
	struct latency_observation *observation
)
{
	if (sample_has_timestamp_rollover(tracker, sample)) {
		tracker->download.baseline_microseconds = sample->download_owd_microseconds;
		tracker->upload.baseline_microseconds = sample->upload_owd_microseconds;
		observation->download_owd_microseconds = sample->download_owd_microseconds;
		observation->download_owd_baseline_microseconds = sample->download_owd_microseconds;
		observation->download_owd_delta_microseconds = 0;
		observation->upload_owd_microseconds = sample->upload_owd_microseconds;
		observation->upload_owd_baseline_microseconds = sample->upload_owd_microseconds;
		observation->upload_owd_delta_microseconds = 0;
		observation->download_owd_delta_ewma_microseconds =
			tracker->download.delta_ewma_microseconds;
		observation->upload_owd_delta_ewma_microseconds =
			tracker->upload.delta_ewma_microseconds;
		observation->timestamp_microseconds = sample->timestamp_microseconds;
		observation->sequence = sample->sequence;
		return;
	}
	observation->download_owd_microseconds = sample->download_owd_microseconds;
	observation->upload_owd_microseconds = sample->upload_owd_microseconds;
	tracker_update_direction(
		&tracker->config,
		&tracker->download,
		sample->download_owd_microseconds,
		&observation->download_owd_baseline_microseconds,
		&observation->download_owd_delta_microseconds
	);
	tracker_update_direction(
		&tracker->config,
		&tracker->upload,
		sample->upload_owd_microseconds,
		&observation->upload_owd_baseline_microseconds,
		&observation->upload_owd_delta_microseconds
	);
	observation->download_owd_delta_ewma_microseconds =
		tracker->download.delta_ewma_microseconds;
	observation->upload_owd_delta_ewma_microseconds = tracker->upload.delta_ewma_microseconds;
	observation->timestamp_microseconds = sample->timestamp_microseconds;
	observation->sequence = sample->sequence;
}

void tracker_update_delta_ewma(
	struct latency_tracker *tracker,
	bool low_load,
	struct latency_observation *observation
)
{
	int64_t alpha = (int64_t)tracker->config.alpha_delta_ewma_per_million;

	/* cake-autorate freezes reflector delay EWMA while either link is busy. */
	if (low_load) {
		tracker->download.delta_ewma_microseconds =
			(alpha * observation->download_owd_delta_microseconds +
			 ((int64_t)MILLION - alpha) * tracker->download.delta_ewma_microseconds) /
			(int64_t)MILLION;
		tracker->upload.delta_ewma_microseconds =
			(alpha * observation->upload_owd_delta_microseconds +
			 ((int64_t)MILLION - alpha) * tracker->upload.delta_ewma_microseconds) /
			(int64_t)MILLION;
	}
	observation->download_owd_delta_ewma_microseconds =
		tracker->download.delta_ewma_microseconds;
	observation->upload_owd_delta_ewma_microseconds = tracker->upload.delta_ewma_microseconds;
}
