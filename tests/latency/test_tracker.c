#define _POSIX_C_SOURCE 200809L

#include "latency/tracker.h"
#include "common/constants.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct latency_tracker_config default_tracker_config = {
	.alpha_baseline_increase_ratio_e6 = 1000U,
	.alpha_baseline_decrease_ratio_e6 = 900000U,
	.alpha_delta_ewma_ratio_e6 = 95000U,
};

static void init_tracker(struct latency_tracker *tracker)
{
	tracker_init(tracker, &default_tracker_config);
}

static struct latency_observation track(struct latency_tracker *tracker, uint32_t round_trip_us)
{
	struct latency_sample sample = {
		.download_owd_us = round_trip_us / 2U,
		.upload_owd_us = round_trip_us / 2U,
	};
	struct latency_observation observation;

	tracker_update(tracker, &sample, &observation);
	tracker_update_delta_ewma(tracker, true, &observation);
	return observation;
}

static void test_first_sample_updates_initialized_baseline(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	observation = track(&tracker, 30000U);

	assert(observation.download_owd_us + observation.upload_owd_us == 30000U);
	assert(observation.download_owd_us == 15000U);
	assert(observation.download_owd_baseline_us == 23500U);
	assert(observation.download_owd_delta_us == -8500);
	assert(observation.download_owd_delta_ewma_us == -808);
}

static void test_configured_alpha_values_are_used(void)
{
	const struct latency_tracker_config config = {
		.alpha_baseline_increase_ratio_e6 = 200000U,
		.alpha_baseline_decrease_ratio_e6 = 500000U,
		.alpha_delta_ewma_ratio_e6 = 500000U,
	};
	struct latency_tracker tracker;
	struct latency_observation observation;

	tracker_init(&tracker, &config);
	observation = track(&tracker, 300000U);
	assert(observation.download_owd_baseline_us == 110000U);
	assert(observation.download_owd_delta_us == 40000);
	assert(observation.download_owd_delta_ewma_us == 20000);

	observation = track(&tracker, 100000U);
	assert(observation.download_owd_baseline_us == 80000U);
	assert(observation.download_owd_delta_us == -30000);
	assert(observation.download_owd_delta_ewma_us == -5000);
}

static void test_delta_ewma_freezes_during_load(void)
{
	struct latency_tracker tracker;
	struct latency_sample sample = {
		.download_owd_us = 120000U,
		.upload_owd_us = 120000U,
	};
	struct latency_observation observation;

	init_tracker(&tracker);
	tracker_update(&tracker, &sample, &observation);
	tracker_update_delta_ewma(&tracker, false, &observation);

	assert(observation.download_owd_delta_us == 19980);
	assert(observation.download_owd_delta_ewma_us == 0);
}

/* A delta too large for the weighted sum restarts the EWMA at the delta. */
static void test_unrepresentable_delta_ewma_restarts(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation = {
		.download_owd_delta_us = INT64_MAX,
		.upload_owd_delta_us = 1000000,
	};

	init_tracker(&tracker);
	tracker_update_delta_ewma(&tracker, true, &observation);
	assert(observation.download_owd_delta_ewma_us == INT64_MAX);
	assert(observation.upload_owd_delta_ewma_us == 95000);
}

static void test_asymmetric_tracker_state_evolves_independently(void)
{
	struct latency_tracker tracker;
	const struct latency_sample sample = {
		.download_owd_us = 200000U,
		.upload_owd_us = 20000U,
	};
	struct latency_observation observation;

	init_tracker(&tracker);
	tracker_update(&tracker, &sample, &observation);
	tracker_update_delta_ewma(&tracker, true, &observation);

	assert(observation.download_owd_baseline_us == 100100U);
	assert(observation.download_owd_delta_us == 99900);
	assert(observation.download_owd_delta_ewma_us == 9491);
	assert(observation.upload_owd_baseline_us == 28000U);
	assert(observation.upload_owd_delta_us == -8000);
	assert(observation.upload_owd_delta_ewma_us == -760);
}

static void test_signed_asymmetric_tracker_handles_one_day_values(void)
{
	struct latency_tracker tracker;
	const struct latency_sample sample = {
		.download_owd_us = -(int64_t)(24U * 60U * MINUTE),
		.upload_owd_us = (int64_t)(24U * 60U * MINUTE),
	};
	struct latency_observation observation;

	init_tracker(&tracker);
	tracker_update(&tracker, &sample, &observation);
	tracker_update_delta_ewma(&tracker, true, &observation);

	assert(observation.download_owd_baseline_us == -INT64_C(77759990000));
	assert(observation.download_owd_delta_us == -INT64_C(8640010000));
	assert(observation.download_owd_delta_ewma_us == -INT64_C(820800950));
	assert(observation.upload_owd_baseline_us == INT64_C(86499900));
	assert(observation.upload_owd_delta_us == INT64_C(86313500100));
	assert(observation.upload_owd_delta_ewma_us == INT64_C(8199782510));
}

static void test_lower_sample_reduces_baseline(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 30000U);
	observation = track(&tracker, 25000U);

	assert(observation.download_owd_baseline_us == 13600U);
	assert(observation.download_owd_delta_us == -1100);
	assert(observation.download_owd_delta_ewma_us == -836);
}

static void test_higher_sample_reports_delta(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 200000U);
	observation = track(&tracker, 240000U);

	assert(observation.download_owd_baseline_us == 100020U);
	assert(observation.download_owd_delta_us == 19980U);
	assert(observation.download_owd_delta_ewma_us == 1898U);
}

static void test_baseline_increases_slowly(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 200000U);
	observation = track(&tracker, 220000U);

	assert(observation.download_owd_baseline_us == 100010U);
	assert(observation.download_owd_delta_us == 9990U);
}

static void test_maximum_rtt_does_not_overflow_delta(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 200000U);
	observation = track(&tracker, UINT32_MAX);

	assert(observation.download_owd_us == 2147483647U);
	assert(observation.download_owd_baseline_us == 2247384U);
	assert(observation.download_owd_delta_us == INT64_C(2145236263));
}

static void test_latency_tracker_reset_discards_measurements(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 30000U);
	tracker_reset(&tracker);
	observation = track(&tracker, 200000U);

	assert(observation.download_owd_baseline_us == 100000U);
	assert(observation.download_owd_delta_ewma_us == 0);
}

static void test_timestamp_rollover_resets_only_timestamp_samples(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;
	struct latency_sample sample = {
		.download_owd_us = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US,
		.upload_owd_us = 0,
		.timestamp_rollover_sensitive = true,
	};

	init_tracker(&tracker);
	tracker.download.baseline_us = 0;
	tracker.upload.baseline_us = 0;
	tracker.download.delta_ewma_us = 100;
	tracker.upload.delta_ewma_us = 200;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.download_owd_delta_us == 0);
	assert(observation.upload_owd_delta_us == 0);
	assert(tracker.download.baseline_us == sample.download_owd_us);
	assert(tracker.upload.baseline_us == sample.upload_owd_us);
	tracker_update_delta_ewma(&tracker, true, &observation);
	assert(tracker.download.delta_ewma_us == 91);
	assert(tracker.upload.delta_ewma_us == 181);

	tracker.download.baseline_us = INT64_MAX;
	tracker.upload.baseline_us = INT64_MIN;
	sample.download_owd_us = INT64_MIN;
	sample.upload_owd_us = INT64_MAX;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.download_owd_delta_us == 0);
	assert(observation.upload_owd_delta_us == 0);
	assert(tracker.download.baseline_us == INT64_MIN);
	assert(tracker.upload.baseline_us == INT64_MAX);
	tracker_update(&tracker, &sample, &observation);
	assert(tracker.download.baseline_us == INT64_MIN);
	assert(tracker.upload.baseline_us == INT64_MAX);
	assert(observation.download_owd_delta_us == 0);
	assert(observation.upload_owd_delta_us == 0);
	tracker.download.delta_ewma_us = 500;
	tracker.upload.delta_ewma_us = 600;
	tracker_update_delta_ewma(&tracker, false, &observation);
	assert(tracker.download.delta_ewma_us == 500);
	assert(tracker.upload.delta_ewma_us == 600);

	tracker_reset(&tracker);
	tracker.download.baseline_us = 0;
	tracker.upload.baseline_us = 0;
	sample.download_owd_us = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US - 1;
	sample.upload_owd_us = 0;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.download_owd_delta_us != 0);
	tracker_reset(&tracker);
	tracker.download.baseline_us = 0;
	tracker.upload.baseline_us = 0;
	sample.download_owd_us = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US - 1;
	sample.upload_owd_us = 2;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.download_owd_delta_us == 0);
	assert(observation.upload_owd_delta_us == 0);
	assert(tracker.download.baseline_us == sample.download_owd_us);
	assert(tracker.upload.baseline_us == sample.upload_owd_us);

	tracker.download.baseline_us = 100;
	tracker.upload.baseline_us = -100;
	sample.download_owd_us = 100 + (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US / 2;
	sample.upload_owd_us = -100 - (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US / 2;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.download_owd_delta_us == 0);
	assert(observation.upload_owd_delta_us == 0);

	tracker.download.baseline_us = 0;
	tracker.upload.baseline_us = 0;
	sample.download_owd_us = 0;
	sample.upload_owd_us = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.download_owd_delta_us == 0);
	assert(observation.upload_owd_delta_us == 0);
	assert(tracker.download.baseline_us == 0);
	assert(tracker.upload.baseline_us == sample.upload_owd_us);

	sample.timestamp_rollover_sensitive = false;
	sample.download_owd_us = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US;
	tracker_reset(&tracker);
	tracker_update(&tracker, &sample, &observation);
	assert(observation.download_owd_delta_us != 0);
}

int main(void)
{
	test_signed_asymmetric_tracker_handles_one_day_values();
	test_first_sample_updates_initialized_baseline();
	test_configured_alpha_values_are_used();
	test_delta_ewma_freezes_during_load();
	test_unrepresentable_delta_ewma_restarts();
	test_asymmetric_tracker_state_evolves_independently();
	test_lower_sample_reduces_baseline();
	test_higher_sample_reports_delta();
	test_baseline_increases_slowly();
	test_maximum_rtt_does_not_overflow_delta();
	test_latency_tracker_reset_discards_measurements();
	test_timestamp_rollover_resets_only_timestamp_samples();

	(void)puts("latency tracker tests passed");
	return 0;
}
