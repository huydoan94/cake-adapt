#include "controller/reflector.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct latency_tracker_config default_tracker_config = {
	.alpha_baseline_increase_per_million = 1000U,
	.alpha_baseline_decrease_per_million = 900000U,
	.alpha_delta_ewma_per_million = 95000U,
};

static void init_tracker(struct latency_tracker *tracker)
{
	tracker_init(tracker, &default_tracker_config);
}

static void test_reflector_health_uses_rolling_offence_window(void)
{
	const struct reflector_health_config config = {
		.response_deadline_us = 1000000U,
		.detection_window = 4U,
		.detection_threshold = 2U,
	};
	struct reflector_health health = { 0 };

	assert(health_init(&health, &config, 1000000U) == 0);
	assert(health_check(&health, 2000000U) == REFLECTOR_HEALTHY);
	assert(health_check(&health, 2000001U) == REFLECTOR_OFFENCE);
	health_record_response(&health, 2500000U);
	assert(health_check(&health, 3000000U) == REFLECTOR_HEALTHY);
	assert(health_check(&health, 3500001U) == REFLECTOR_MISBEHAVING);
	health_record_response(&health, 4000000U);
	assert(health_check(&health, 4000000U) == REFLECTOR_MISBEHAVING);
	assert(health_check(&health, 4000000U) == REFLECTOR_HEALTHY);
	health_cleanup(&health);
}

static void test_reflector_health_reset_clears_offences(void)
{
	const struct reflector_health_config config = {
		.response_deadline_us = 100U,
		.detection_window = 2U,
		.detection_threshold = 1U,
	};
	struct reflector_health health = { 0 };

	assert(health_init(&health, &config, 100U) == 0);
	assert(health_check(&health, 201U) == REFLECTOR_MISBEHAVING);
	health_reset(&health, 300U);
	assert(health_check(&health, 400U) == REFLECTOR_HEALTHY);
	assert(health.offence_count == 0U);
	health_cleanup(&health);
}

static void test_reflector_comparison_uses_active_order(void)
{
	struct latency_tracker trackers[3];
	const size_t order[] = { 2U, 0U };
	struct reflector_comparison comparisons[2];
	size_t index;

	for (index = 0U; index < 3U; index++)
		init_tracker(&trackers[index]);
	trackers[0].download.baseline_us = trackers[0].upload.baseline_us = 90000U;
	trackers[0].download.delta_ewma_us = trackers[0].upload.delta_ewma_us = -100;
	trackers[1].download.baseline_us = trackers[1].upload.baseline_us = 1U;
	trackers[1].download.delta_ewma_us = trackers[1].upload.delta_ewma_us = -1000;
	trackers[2].download.baseline_us = trackers[2].upload.baseline_us = 100000U;
	trackers[2].download.delta_ewma_us = trackers[2].upload.delta_ewma_us = 400;

	reflector_compare(trackers, order, 2U, comparisons);
	assert(comparisons[0].minimum_sum_owd_baselines_us == 180000U);
	assert(comparisons[0].sum_owd_baselines_us == 200000U);
	assert(comparisons[0].sum_owd_baselines_delta_us == 20000U);
	assert(comparisons[0].minimum_download_delta_ewma_us == -100);
	assert(comparisons[0].download_delta_ewma_us == 400);
	assert(comparisons[0].download_delta_ewma_delta_us == 500);
	assert(comparisons[1].sum_owd_baselines_delta_us == 0U);
	assert(comparisons[1].minimum_download_delta_ewma_us == -100);
	assert(comparisons[1].download_delta_ewma_us == -100);
	assert(comparisons[1].download_delta_ewma_delta_us == 0);
}

static void test_reflector_comparison_keeps_directional_minima(void)
{
	struct latency_tracker trackers[3];
	const size_t order[] = { 2U, 0U };
	struct reflector_comparison comparisons[2];
	size_t index;

	for (index = 0U; index < 3U; index++)
		init_tracker(&trackers[index]);
	trackers[0].download.baseline_us = 50000U;
	trackers[0].upload.baseline_us = 170000U;
	trackers[0].download.delta_ewma_us = 100;
	trackers[0].upload.delta_ewma_us = -500;
	trackers[1].download.baseline_us = 1U;
	trackers[1].upload.baseline_us = 1U;
	trackers[1].download.delta_ewma_us = -1000;
	trackers[1].upload.delta_ewma_us = -1000;
	trackers[2].download.baseline_us = 100000U;
	trackers[2].upload.baseline_us = 80000U;
	trackers[2].download.delta_ewma_us = -200;
	trackers[2].upload.delta_ewma_us = 400;

	reflector_compare(trackers, order, 2U, comparisons);
	assert(comparisons[0].minimum_sum_owd_baselines_us == 180000U);
	assert(comparisons[0].sum_owd_baselines_us == 180000U);
	assert(comparisons[0].minimum_download_delta_ewma_us == -200);
	assert(comparisons[0].download_delta_ewma_delta_us == 0);
	assert(comparisons[0].minimum_upload_delta_ewma_us == -500);
	assert(comparisons[0].upload_delta_ewma_delta_us == 900);
	assert(comparisons[1].sum_owd_baselines_delta_us == 40000U);
	assert(comparisons[1].download_delta_ewma_delta_us == 300);
	assert(comparisons[1].upload_delta_ewma_delta_us == 0);
}

static void test_reflector_comparison_preserves_signed_baselines(void)
{
	struct latency_tracker trackers[2];
	const size_t order[] = { 0U, 1U };
	struct reflector_comparison comparisons[2];

	init_tracker(&trackers[0]);
	init_tracker(&trackers[1]);
	trackers[0].download.baseline_us = -INT64_C(40000000000);
	trackers[0].upload.baseline_us = INT64_C(10000000000);
	trackers[0].download.delta_ewma_us = INT64_C(5000000000);
	trackers[0].upload.delta_ewma_us = -INT64_C(6000000000);
	trackers[1].download.baseline_us = -INT64_C(50000000000);
	trackers[1].upload.baseline_us = -INT64_C(20000000000);
	trackers[1].download.delta_ewma_us = -INT64_C(7000000000);
	trackers[1].upload.delta_ewma_us = INT64_C(8000000000);

	reflector_compare(trackers, order, 2U, comparisons);
	assert(comparisons[0].minimum_sum_owd_baselines_us == -INT64_C(70000000000));
	assert(comparisons[0].sum_owd_baselines_us == -INT64_C(30000000000));
	assert(comparisons[0].sum_owd_baselines_delta_us == UINT64_C(40000000000));
	assert(comparisons[0].download_delta_ewma_delta_us == INT64_C(12000000000));
	assert(comparisons[0].upload_delta_ewma_delta_us == 0);
}

static void test_reflector_comparison_separates_all_minima(void)
{
	struct latency_tracker trackers[3];
	const size_t order[] = { 0U, 1U, 2U };
	struct reflector_comparison comparisons[3];
	size_t index;

	for (index = 0U; index < 3U; index++)
		init_tracker(&trackers[index]);
	trackers[0].download.baseline_us = 20U;
	trackers[0].upload.baseline_us = 120U;
	trackers[0].download.delta_ewma_us = 50;
	trackers[0].upload.delta_ewma_us = 9;
	trackers[1].download.baseline_us = 90U;
	trackers[1].upload.baseline_us = 70U;
	trackers[1].download.delta_ewma_us = -5;
	trackers[1].upload.delta_ewma_us = 30;
	trackers[2].download.baseline_us = 200U;
	trackers[2].upload.baseline_us = 100U;
	trackers[2].download.delta_ewma_us = 10;
	trackers[2].upload.delta_ewma_us = -10;

	reflector_compare(trackers, order, 3U, comparisons);
	assert(comparisons[0].minimum_sum_owd_baselines_us == 140);
	assert(comparisons[0].minimum_download_delta_ewma_us == -5);
	assert(comparisons[0].minimum_upload_delta_ewma_us == -10);
	assert(comparisons[0].sum_owd_baselines_delta_us == 0U);
	assert(comparisons[1].sum_owd_baselines_delta_us == 20U);
	assert(comparisons[1].download_delta_ewma_delta_us == 0);
	assert(comparisons[2].upload_delta_ewma_delta_us == 0);
}

static void test_reflector_comparison_saturates_signed_extremes(void)
{
	struct latency_tracker trackers[2];
	const size_t order[] = { 0U, 1U };
	struct reflector_comparison comparisons[2];

	init_tracker(&trackers[0]);
	init_tracker(&trackers[1]);
	trackers[0].download.baseline_us = INT64_MAX;
	trackers[0].upload.baseline_us = INT64_MAX;
	trackers[0].download.delta_ewma_us = INT64_MAX;
	trackers[0].upload.delta_ewma_us = INT64_MAX;
	trackers[1].download.baseline_us = INT64_MIN;
	trackers[1].upload.baseline_us = INT64_MIN;
	trackers[1].download.delta_ewma_us = INT64_MIN;
	trackers[1].upload.delta_ewma_us = INT64_MIN;

	reflector_compare(trackers, order, 2U, comparisons);
	assert(comparisons[0].sum_owd_baselines_us == INT64_MAX);
	assert(comparisons[0].sum_owd_baselines_delta_us == UINT64_MAX);
	assert(comparisons[0].download_delta_ewma_delta_us == INT64_MAX);
	assert(comparisons[0].upload_delta_ewma_delta_us == INT64_MAX);
	assert(comparisons[1].sum_owd_baselines_us == INT64_MIN);
}

static void test_reflector_rotation_retains_or_discards_identity_state(void)
{
	struct latency_tracker trackers[3];
	size_t order[] = { 0U, 1U, 2U };
	size_t index;

	for (index = 0U; index < 3U; index++)
		init_tracker(&trackers[index]);
	trackers[0].download.baseline_us = 123;
	trackers[0].upload.baseline_us = 456;
	trackers[0].download.delta_ewma_us = 7;
	trackers[0].upload.delta_ewma_us = 8;

	/* Retention leaves state on the immutable reflector identity. */
	reflector_rotate(order, 3U, 1U, 0U);
	reflector_rotate(order, 3U, 1U, 0U);
	reflector_rotate(order, 3U, 1U, 0U);
	assert(order[0] == 0U);
	assert(trackers[0].download.baseline_us == 123);
	assert(trackers[0].upload.baseline_us == 456);
	assert(trackers[0].download.delta_ewma_us == 7);
	assert(trackers[0].upload.delta_ewma_us == 8);

	/* Discarding resets the departing identity before its next active turn. */
	tracker_reset(&trackers[0]);
	reflector_rotate(order, 3U, 1U, 0U);
	reflector_rotate(order, 3U, 1U, 0U);
	reflector_rotate(order, 3U, 1U, 0U);
	assert(order[0] == 0U);
	assert(trackers[0].download.baseline_us == INT64_C(100000));
	assert(trackers[0].upload.baseline_us == INT64_C(100000));
	assert(trackers[0].download.delta_ewma_us == 0);
	assert(trackers[0].upload.delta_ewma_us == 0);
}

static void test_reflector_rotation_uses_first_standby(void)
{
	size_t order[] = { 0U, 1U, 2U, 3U, 4U };

	reflector_rotate(order, 5U, 2U, 1U);
	assert(order[0] == 0U);
	assert(order[1] == 2U);
	assert(order[2] == 3U);
	assert(order[3] == 4U);
	assert(order[4] == 1U);
}

int main(void)
{
	test_reflector_comparison_preserves_signed_baselines();
	test_reflector_health_uses_rolling_offence_window();
	test_reflector_health_reset_clears_offences();
	test_reflector_comparison_uses_active_order();
	test_reflector_comparison_separates_all_minima();
	test_reflector_comparison_saturates_signed_extremes();
	test_reflector_rotation_retains_or_discards_identity_state();
	test_reflector_rotation_uses_first_standby();
	test_reflector_comparison_keeps_directional_minima();

	(void)puts("reflector tests passed");
	return 0;
}
