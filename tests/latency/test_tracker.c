#define _POSIX_C_SOURCE 200809L

#include "latency/tracker.h"
#include "constants/latency.h"
#include "constants/units.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The low_load argument: cake-autorate updates the delay EWMA only on a quiet link. */
static const bool link_quiet = true;
static const bool link_busy = false;

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
		.owd_us[DIRECTION_DOWNLOAD] = round_trip_us / 2U,
		.owd_us[DIRECTION_UPLOAD] = round_trip_us / 2U,
	};
	struct latency_observation observation;

	tracker_update(tracker, &sample, &observation);
	tracker_update_delta_ewma(tracker, link_quiet, &observation);
	return observation;
}

static void test_first_sample_updates_initialized_baseline(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	observation = track(&tracker, 30000U);

	assert(observation.owd_us[DIRECTION_DOWNLOAD] + observation.owd_us[DIRECTION_UPLOAD] ==
	       30000U);
	assert(observation.owd_us[DIRECTION_DOWNLOAD] == 15000U);
	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 23500U);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == -8500);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == -808);
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
	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 110000U);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 40000);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == 20000);

	observation = track(&tracker, 100000U);
	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 80000U);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == -30000);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == -5000);
}

static void test_delta_ewma_freezes_during_load(void)
{
	struct latency_tracker tracker;
	struct latency_sample sample = {
		.owd_us[DIRECTION_DOWNLOAD] = 120000U,
		.owd_us[DIRECTION_UPLOAD] = 120000U,
	};
	struct latency_observation observation;

	init_tracker(&tracker);
	tracker_update(&tracker, &sample, &observation);
	tracker_update_delta_ewma(&tracker, link_busy, &observation);

	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 19980);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == 0);
}

/* A delta too large for the weighted sum restarts the EWMA at the delta. */
static void test_unrepresentable_delta_ewma_restarts(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation = {
		.owd_delta_us[DIRECTION_DOWNLOAD] = INT64_MAX,
		.owd_delta_us[DIRECTION_UPLOAD] = 1000000,
	};

	init_tracker(&tracker);
	tracker_update_delta_ewma(&tracker, link_quiet, &observation);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == INT64_MAX);
	assert(observation.owd_delta_ewma_us[DIRECTION_UPLOAD] == 95000);
}

static void test_asymmetric_tracker_state_evolves_independently(void)
{
	struct latency_tracker tracker;
	const struct latency_sample sample = {
		.owd_us[DIRECTION_DOWNLOAD] = 200000U,
		.owd_us[DIRECTION_UPLOAD] = 20000U,
	};
	struct latency_observation observation;

	init_tracker(&tracker);
	tracker_update(&tracker, &sample, &observation);
	tracker_update_delta_ewma(&tracker, link_quiet, &observation);

	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 100100U);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 99900);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == 9491);
	assert(observation.owd_baseline_us[DIRECTION_UPLOAD] == 28000U);
	assert(observation.owd_delta_us[DIRECTION_UPLOAD] == -8000);
	assert(observation.owd_delta_ewma_us[DIRECTION_UPLOAD] == -760);
}

static void test_signed_asymmetric_tracker_handles_one_day_values(void)
{
	struct latency_tracker tracker;
	const struct latency_sample sample = {
		.owd_us[DIRECTION_DOWNLOAD] = -(int64_t)(24U * 60U * MINUTE),
		.owd_us[DIRECTION_UPLOAD] = (int64_t)(24U * 60U * MINUTE),
	};
	struct latency_observation observation;

	init_tracker(&tracker);
	tracker_update(&tracker, &sample, &observation);
	tracker_update_delta_ewma(&tracker, link_quiet, &observation);

	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == -INT64_C(77759990000));
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == -INT64_C(8640010000));
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == -INT64_C(820800950));
	assert(observation.owd_baseline_us[DIRECTION_UPLOAD] == INT64_C(86499900));
	assert(observation.owd_delta_us[DIRECTION_UPLOAD] == INT64_C(86313500100));
	assert(observation.owd_delta_ewma_us[DIRECTION_UPLOAD] == INT64_C(8199782510));
}

static void test_lower_sample_reduces_baseline(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 30000U);
	observation = track(&tracker, 25000U);

	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 13600U);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == -1100);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == -836);
}

static void test_higher_sample_reports_delta(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 200000U);
	observation = track(&tracker, 240000U);

	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 100020U);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 19980U);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == 1898U);
}

static void test_baseline_increases_slowly(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 200000U);
	observation = track(&tracker, 220000U);

	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 100010U);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 9990U);
}

static void test_maximum_rtt_does_not_overflow_delta(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 200000U);
	observation = track(&tracker, UINT32_MAX);

	assert(observation.owd_us[DIRECTION_DOWNLOAD] == 2147483647U);
	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 2247384U);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == INT64_C(2145236263));
}

static void test_latency_tracker_reset_discards_measurements(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;

	init_tracker(&tracker);
	(void)track(&tracker, 30000U);
	tracker_reset(&tracker);
	observation = track(&tracker, 200000U);

	assert(observation.owd_baseline_us[DIRECTION_DOWNLOAD] == 100000U);
	assert(observation.owd_delta_ewma_us[DIRECTION_DOWNLOAD] == 0);
}

static void test_timestamp_rollover_resets_only_timestamp_samples(void)
{
	struct latency_tracker tracker;
	struct latency_observation observation;
	struct latency_sample sample = {
		.owd_us[DIRECTION_DOWNLOAD] = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US,
		.owd_us[DIRECTION_UPLOAD] = 0,
		.timestamp_rollover_sensitive = true,
	};

	init_tracker(&tracker);
	tracker.directions[DIRECTION_DOWNLOAD].baseline_us = 0;
	tracker.directions[DIRECTION_UPLOAD].baseline_us = 0;
	tracker.directions[DIRECTION_DOWNLOAD].delta_ewma_us = 100;
	tracker.directions[DIRECTION_UPLOAD].delta_ewma_us = 200;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 0);
	assert(observation.owd_delta_us[DIRECTION_UPLOAD] == 0);
	assert(tracker.directions[DIRECTION_DOWNLOAD].baseline_us ==
	       sample.owd_us[DIRECTION_DOWNLOAD]);
	assert(tracker.directions[DIRECTION_UPLOAD].baseline_us == sample.owd_us[DIRECTION_UPLOAD]);
	tracker_update_delta_ewma(&tracker, link_quiet, &observation);
	assert(tracker.directions[DIRECTION_DOWNLOAD].delta_ewma_us == 91);
	assert(tracker.directions[DIRECTION_UPLOAD].delta_ewma_us == 181);

	tracker.directions[DIRECTION_DOWNLOAD].baseline_us = INT64_MAX;
	tracker.directions[DIRECTION_UPLOAD].baseline_us = INT64_MIN;
	sample.owd_us[DIRECTION_DOWNLOAD] = INT64_MIN;
	sample.owd_us[DIRECTION_UPLOAD] = INT64_MAX;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 0);
	assert(observation.owd_delta_us[DIRECTION_UPLOAD] == 0);
	assert(tracker.directions[DIRECTION_DOWNLOAD].baseline_us == INT64_MIN);
	assert(tracker.directions[DIRECTION_UPLOAD].baseline_us == INT64_MAX);
	tracker_update(&tracker, &sample, &observation);
	assert(tracker.directions[DIRECTION_DOWNLOAD].baseline_us == INT64_MIN);
	assert(tracker.directions[DIRECTION_UPLOAD].baseline_us == INT64_MAX);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 0);
	assert(observation.owd_delta_us[DIRECTION_UPLOAD] == 0);
	tracker.directions[DIRECTION_DOWNLOAD].delta_ewma_us = 500;
	tracker.directions[DIRECTION_UPLOAD].delta_ewma_us = 600;
	tracker_update_delta_ewma(&tracker, link_busy, &observation);
	assert(tracker.directions[DIRECTION_DOWNLOAD].delta_ewma_us == 500);
	assert(tracker.directions[DIRECTION_UPLOAD].delta_ewma_us == 600);

	tracker_reset(&tracker);
	tracker.directions[DIRECTION_DOWNLOAD].baseline_us = 0;
	tracker.directions[DIRECTION_UPLOAD].baseline_us = 0;
	sample.owd_us[DIRECTION_DOWNLOAD] = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US - 1;
	sample.owd_us[DIRECTION_UPLOAD] = 0;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] != 0);
	tracker_reset(&tracker);
	tracker.directions[DIRECTION_DOWNLOAD].baseline_us = 0;
	tracker.directions[DIRECTION_UPLOAD].baseline_us = 0;
	sample.owd_us[DIRECTION_DOWNLOAD] = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US - 1;
	sample.owd_us[DIRECTION_UPLOAD] = 2;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 0);
	assert(observation.owd_delta_us[DIRECTION_UPLOAD] == 0);
	assert(tracker.directions[DIRECTION_DOWNLOAD].baseline_us ==
	       sample.owd_us[DIRECTION_DOWNLOAD]);
	assert(tracker.directions[DIRECTION_UPLOAD].baseline_us == sample.owd_us[DIRECTION_UPLOAD]);

	tracker.directions[DIRECTION_DOWNLOAD].baseline_us = 100;
	tracker.directions[DIRECTION_UPLOAD].baseline_us = -100;
	sample.owd_us[DIRECTION_DOWNLOAD] = 100 + (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US / 2;
	sample.owd_us[DIRECTION_UPLOAD] = -100 - (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US / 2;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 0);
	assert(observation.owd_delta_us[DIRECTION_UPLOAD] == 0);

	tracker.directions[DIRECTION_DOWNLOAD].baseline_us = 0;
	tracker.directions[DIRECTION_UPLOAD].baseline_us = 0;
	sample.owd_us[DIRECTION_DOWNLOAD] = 0;
	sample.owd_us[DIRECTION_UPLOAD] = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US;
	tracker_update(&tracker, &sample, &observation);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] == 0);
	assert(observation.owd_delta_us[DIRECTION_UPLOAD] == 0);
	assert(tracker.directions[DIRECTION_DOWNLOAD].baseline_us == 0);
	assert(tracker.directions[DIRECTION_UPLOAD].baseline_us == sample.owd_us[DIRECTION_UPLOAD]);

	sample.timestamp_rollover_sensitive = false;
	sample.owd_us[DIRECTION_DOWNLOAD] = (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_US;
	tracker_reset(&tracker);
	tracker_update(&tracker, &sample, &observation);
	assert(observation.owd_delta_us[DIRECTION_DOWNLOAD] != 0);
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
