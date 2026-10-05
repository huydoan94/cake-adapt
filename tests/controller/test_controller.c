#include "controller/controller.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MEBABIT 1000000U

/* Zero allows every allocation; otherwise the Nth calloc() fails. */
static unsigned int failing_allocation;
static unsigned int allocations;

void *__real_calloc(size_t count, size_t size);

void *__wrap_calloc(size_t count, size_t size)
{
	if (failing_allocation != 0U && ++allocations == failing_allocation)
		return NULL;
	return __real_calloc(count, size);
}

static struct controller_config default_config(void)
{
	return (struct controller_config){
		.download = { .average_delay_maximum_adjust_up_microseconds = 10000U,
			      .delay_threshold_microseconds = 30000U,
			      .average_delay_maximum_adjust_down_microseconds = 60000U, },
		.upload = { .average_delay_maximum_adjust_up_microseconds = 10000U,
			    .delay_threshold_microseconds = 30000U,
			    .average_delay_maximum_adjust_down_microseconds = 60000U, },
		.bufferbloat_detection_window = 6U,
		.bufferbloat_detection_threshold = 3U,
		.rate_minimum_adjust_down_bufferbloat_per_thousand = 990U,
		.rate_maximum_adjust_down_bufferbloat_per_thousand = 750U,
		.rate_minimum_adjust_up_high_load_per_thousand = 1000U,
		.rate_maximum_adjust_up_high_load_per_thousand = 1040U,
		.rate_adjust_down_low_load_per_thousand = 990U,
		.rate_adjust_up_low_load_per_thousand = 1010U,
		.high_load_threshold_percent = 75U,
		.bufferbloat_refractory_period_microseconds = 300000U,
		.decay_refractory_period_microseconds = 1000000U
	};
}

static struct controller_config adjusting_config(void)
{
	struct controller_config config = default_config();

	config.download.adjust = true;
	config.download.minimum_rate_bits_per_second = 5U * MEBABIT;
	config.download.base_rate_bits_per_second = 8U * MEBABIT;
	config.download.maximum_rate_bits_per_second = 12U * MEBABIT;
	config.upload.adjust = true;
	config.upload.minimum_rate_bits_per_second = 5U * MEBABIT;
	config.upload.base_rate_bits_per_second = 8U * MEBABIT;
	config.upload.maximum_rate_bits_per_second = 12U * MEBABIT;
	return config;
}

static struct controller_input input_with_rates(
	uint64_t download_rate,
	uint64_t download_limit,
	uint64_t upload_rate,
	uint64_t upload_limit
)
{
	return (struct controller_input){
		.download = { .valid = true,
			      .traffic_sample_id = 1U,
			      .traffic_rate_bits_per_second = download_rate,
			      .cake_rate_bits_per_second = download_limit, },
		.upload = { .valid = true,
			    .traffic_sample_id = 1U,
			    .traffic_rate_bits_per_second = upload_rate,
			    .cake_rate_bits_per_second = upload_limit, },
		.download_latency = { .valid = true, .owd_delta_microseconds = 0 },
		.upload_latency = { .valid = true, .owd_delta_microseconds = 0 },
		.timestamp_microseconds = 1000001U
	};
}

static void set_latency_delta(struct controller_input *input, int64_t delta_microseconds)
{
	input->download_latency.owd_delta_microseconds = delta_microseconds;
	input->upload_latency.owd_delta_microseconds = delta_microseconds;
}

static void update_repeatedly(
	struct controller *controller,
	const struct controller_input *input,
	struct controller_output *output,
	unsigned int count
)
{
	unsigned int index;

	for (index = 0U; index < count; index++)
		controller_update(controller, input, output);
}

static void accept_rates(struct controller_input *input, const struct controller_output *output)
{
	input->download.cake_rate_bits_per_second = output->download.rate_bits_per_second;
	input->upload.cake_rate_bits_per_second = output->upload.rate_bits_per_second;
}

static void init_controller(struct controller *controller, const struct controller_config *config)
{
	assert(controller_init(controller, config) == 0);
}

static void test_initial_state_is_unknown(void)
{
	struct controller controller;
	const struct controller_config config = default_config();

	init_controller(&controller, &config);

	assert(controller.download.state == CONTROLLER_LINE_UNKNOWN);
	assert(controller.upload.state == CONTROLLER_LINE_UNKNOWN);
	assert(controller.download.congestion == CONTROLLER_CONGESTION_UNKNOWN);
	assert(controller.upload.congestion == CONTROLLER_CONGESTION_UNKNOWN);
	controller_close(&controller);
}

static void test_low_load_is_below_capacity(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);

	assert(output.download.state == CONTROLLER_LINE_BELOW_CAPACITY);
	assert(output.upload.state == CONTROLLER_LINE_BELOW_CAPACITY);
	assert(output.download.congestion == CONTROLLER_CONGESTION_CLEAR);
	assert(output.upload.congestion == CONTROLLER_CONGESTION_CLEAR);
	assert(!output.download.rate_changed);
	assert(!output.upload.rate_changed);
	controller_close(&controller);
}

static void test_sustained_download_is_saturated(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input input =
		input_with_rates(7200000U, 8U * MEBABIT, 100000U, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	assert(output.download.state == CONTROLLER_LINE_UNKNOWN);
	controller_update(&controller, &input, &output);
	assert(output.download.state == CONTROLLER_LINE_UNKNOWN);
	controller_update(&controller, &input, &output);

	assert(output.download.state == CONTROLLER_LINE_SATURATED);
	assert(output.download.state_changed);
	assert(output.upload.state == CONTROLLER_LINE_BELOW_CAPACITY);
	controller_close(&controller);
}

static void test_brief_burst_does_not_saturate(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input high =
		input_with_rates(8U * MEBABIT, 8U * MEBABIT, 0U, 8U * MEBABIT);
	struct controller_input low =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 0U, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	update_repeatedly(&controller, &high, &output, 2U);
	controller_update(&controller, &low, &output);

	assert(output.download.state == CONTROLLER_LINE_BELOW_CAPACITY);
	controller_close(&controller);
}

static void test_line_hysteresis_and_recovery(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input high =
		input_with_rates(8U * MEBABIT, 8U * MEBABIT, 0U, 8U * MEBABIT);
	struct controller_input middle = input_with_rates(6800000U, 8U * MEBABIT, 0U, 8U * MEBABIT);
	struct controller_input low =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 0U, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	update_repeatedly(&controller, &high, &output, 3U);
	update_repeatedly(&controller, &middle, &output, 5U);
	assert(output.download.state == CONTROLLER_LINE_SATURATED);

	update_repeatedly(&controller, &low, &output, 2U);
	assert(output.download.state == CONTROLLER_LINE_SATURATED);
	controller_update(&controller, &low, &output);
	assert(output.download.state == CONTROLLER_LINE_BELOW_CAPACITY);
	controller_close(&controller);
}

static void test_invalid_direction_returns_to_unknown(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.download.valid = false;
	controller_update(&controller, &input, &output);

	assert(output.download.state == CONTROLLER_LINE_UNKNOWN);
	assert(output.download.state_changed);
	controller_close(&controller);
}

static void test_three_of_six_delays_detect_bufferbloat(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	set_latency_delta(&input, 30001);
	init_controller(&controller, &config);
	update_repeatedly(&controller, &input, &output, 2U);
	assert(output.download.congestion == CONTROLLER_CONGESTION_CLEAR);
	controller_update(&controller, &input, &output);

	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	assert(output.upload.congestion == CONTROLLER_CONGESTION_DETECTED);
	assert(output.download.congestion_changed);
	assert(output.download.delayed_sample_count == 3U);
	assert(output.upload.delayed_sample_count == 3U);
	assert(controller.download.delay_sum_microseconds == 90003U);
	assert(controller.upload.delay_sum_microseconds == 90003U);
	assert(output.download.average_delay_microseconds == 15000U);
	assert(output.upload.average_delay_microseconds == 15000U);
	controller_close(&controller);
}

static void test_below_baseline_delay_remains_signed(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	set_latency_delta(&input, -3000);
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);

	assert(controller.download.delay_sum_microseconds == -3000);
	assert(controller.upload.delay_sum_microseconds == -3000);
	assert(output.download.average_delay_microseconds == -500);
	assert(output.upload.average_delay_microseconds == -500);
	assert(output.download.delayed_sample_count == 0U);
	assert(output.upload.delayed_sample_count == 0U);
	controller_close(&controller);
}

static void test_delay_window_clears_after_old_delays_expire(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	set_latency_delta(&input, 30001);
	init_controller(&controller, &config);
	update_repeatedly(&controller, &input, &output, 3U);
	set_latency_delta(&input, 0);
	update_repeatedly(&controller, &input, &output, 3U);
	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	controller_update(&controller, &input, &output);

	assert(output.download.congestion == CONTROLLER_CONGESTION_CLEAR);
	assert(output.download.congestion_changed);
	controller_close(&controller);
}

static void test_missing_probe_holds_delay_window(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	set_latency_delta(&input, 30001);
	init_controller(&controller, &config);
	update_repeatedly(&controller, &input, &output, 2U);
	input.download_latency.valid = false;
	input.upload_latency.valid = false;
	controller_update(&controller, &input, &output);
	assert(output.download.congestion == CONTROLLER_CONGESTION_UNKNOWN);
	input.download_latency.valid = true;
	input.upload_latency.valid = true;
	controller_update(&controller, &input, &output);

	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	controller_close(&controller);
}

static void test_configured_delay_window_and_direction_thresholds(void)
{
	struct controller controller;
	struct controller_config config = default_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.bufferbloat_detection_window = 4U;
	config.bufferbloat_detection_threshold = 2U;
	config.download.delay_threshold_microseconds = 10000U;
	config.upload.delay_threshold_microseconds = 20000U;
	set_latency_delta(&input, 15000);
	init_controller(&controller, &config);
	update_repeatedly(&controller, &input, &output, 2U);

	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	assert(output.download.delayed_sample_count == 2U);
	assert(output.download.average_delay_microseconds == 7500);
	assert(output.upload.congestion == CONTROLLER_CONGESTION_CLEAR);
	assert(output.upload.delayed_sample_count == 0U);
	controller_close(&controller);
}

static void test_invalid_delay_window_is_rejected(void)
{
	struct controller controller;
	struct controller_config config = default_config();

	config.bufferbloat_detection_window = 0U;
	assert(controller_init(&controller, &config) != 0);

	config.bufferbloat_detection_window = 2U;
	config.bufferbloat_detection_threshold = 3U;
	assert(controller_init(&controller, &config) != 0);
}

static void test_allocation_failure_releases_every_window(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	unsigned int failure;

	/* Each direction owns two windows; leak checking covers every failure point. */
	for (failure = 1U; failure <= 4U; failure++) {
		failing_allocation = failure;
		allocations = 0U;
		assert(controller_init(&controller, &config) != 0);
		assert(controller.download.delay_samples == NULL);
		assert(controller.download.delayed_samples == NULL);
		assert(controller.upload.delay_samples == NULL);
		assert(controller.upload.delayed_samples == NULL);
	}
	failing_allocation = 0U;
}

static void test_initial_rate_is_baseline(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 6U * MEBABIT, 1U * MEBABIT, 6U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);

	assert(output.download.rate_bits_per_second == 8U * MEBABIT);
	assert(output.upload.rate_bits_per_second == 8U * MEBABIT);
	assert(output.download.rate_changed);
	assert(output.download.rate_reason == CONTROLLER_RATE_INITIAL);
	controller_close(&controller);
}

static void test_initial_rate_is_written_even_when_cake_holds_it(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8U * MEBABIT);
	assert(output.download.rate_changed);
	assert(output.download.rate_reason == CONTROLLER_RATE_INITIAL);
	assert(output.upload.rate_changed);

	/* Afterwards an equal rate is not rewritten. */
	controller_update(&controller, &input, &output);
	assert(!output.download.rate_changed);
	assert(!output.upload.rate_changed);
	controller_close(&controller);
}

static void test_initial_rate_waits_for_valid_qdisc_input(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 6U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	input.download.valid = false;
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	assert(!output.download.rate_changed);
	assert(controller.download.initial_rate_pending);

	input.download.valid = true;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8U * MEBABIT);
	assert(output.download.rate_changed);
	assert(output.download.rate_reason == CONTROLLER_RATE_INITIAL);
	controller_close(&controller);
}

static void test_high_load_increases_rate_four_percent(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(6080000U, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 300001U;
	controller_update(&controller, &input, &output);

	assert(output.download.rate_bits_per_second == 8320000U);
	assert(output.download.rate_changed);
	assert(output.download.rate_reason == CONTROLLER_RATE_HIGH_LOAD);
	controller_close(&controller);
}

static void test_high_load_consumes_each_direction_sample_once(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(20U * MEBABIT, 8U * MEBABIT, 20U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 300001U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8320000U);
	assert(output.upload.rate_bits_per_second == 8320000U);
	accept_rates(&input, &output);

	/* Six reflector replies must not multiply one achieved-rate measurement. */
	for (unsigned int index = 0U; index < 6U; index++) {
		input.timestamp_microseconds += 50000U;
		controller_update(&controller, &input, &output);
		assert(!output.download.rate_changed);
		assert(!output.upload.rate_changed);
	}
	input.download.traffic_sample_id++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8652000U);
	assert(!output.upload.rate_changed);
	accept_rates(&input, &output);
	input.upload.traffic_sample_id++;
	controller_update(&controller, &input, &output);
	assert(!output.download.rate_changed);
	assert(output.upload.rate_bits_per_second == 8652000U);
	controller_close(&controller);
}

static void test_invalid_input_preserves_fresh_sample(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(20U * MEBABIT, 8U * MEBABIT, 20U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 300001U;
	input.download_latency.valid = false;
	input.upload_latency.valid = false;
	controller_update(&controller, &input, &output);
	assert(controller.download.last_increase_sample_id == 0U);
	input.download_latency.valid = true;
	input.upload_latency.valid = true;
	input.download.valid = false;
	controller_update(&controller, &input, &output);
	assert(controller.download.last_increase_sample_id == 0U);
	assert(output.upload.rate_bits_per_second == 8320000U);
	accept_rates(&input, &output);
	input.download.valid = true;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8320000U);
	assert(!output.upload.rate_changed);
	controller_close(&controller);
}

static void test_noop_increase_consumes_sample(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(20U * MEBABIT, 8U * MEBABIT, 20U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	/* A factor of one and a maximum-rate clamp both consume the opportunity. */
	config.rate_maximum_adjust_up_high_load_per_thousand = 1000U;
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 300001U;
	controller_update(&controller, &input, &output);
	assert(!output.download.rate_changed);
	assert(controller.download.last_increase_sample_id == 1U);
	controller.config.rate_maximum_adjust_up_high_load_per_thousand = 1040U;
	controller_update(&controller, &input, &output);
	assert(!output.download.rate_changed);

	controller.download.shaper_rate_bits_per_second = 12U * MEBABIT;
	input.download.cake_rate_bits_per_second = 12U * MEBABIT;
	input.download.traffic_sample_id++;
	controller_update(&controller, &input, &output);
	assert(!output.download.rate_changed);
	assert(controller.download.last_increase_sample_id == 2U);

	/* Consuming the load sample must not suppress subsequent congestion cuts. */
	set_latency_delta(&input, 100000);
	update_repeatedly(&controller, &input, &output, 6U);
	assert(output.download.rate_bits_per_second < 12U * MEBABIT);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 300001U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_changed);
	assert(output.download.rate_reason == CONTROLLER_RATE_CONGESTION);
	assert(controller.download.last_increase_sample_id == 2U);
	controller_close(&controller);
}

static void test_high_load_waits_for_congestion_refractory_period(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(6080000U, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 299999U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8U * MEBABIT);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8U * MEBABIT);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8320000U);
	assert(output.download.rate_reason == CONTROLLER_RATE_HIGH_LOAD);
	controller_close(&controller);
}

static void test_configured_high_load_adjustment_is_used(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(4080000U, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.high_load_threshold_percent = 50U;
	config.rate_maximum_adjust_up_high_load_per_thousand = 1100U;
	config.bufferbloat_refractory_period_microseconds = 10U;
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 11U;
	controller_update(&controller, &input, &output);

	assert(output.download.rate_bits_per_second == 8800000U);
	assert(output.download.rate_reason == CONTROLLER_RATE_HIGH_LOAD);
	controller_close(&controller);
}

static void test_severe_bufferbloat_reduces_both_rates(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	set_latency_delta(&input, 120000);
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 150001U;
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 150001U;
	controller_update(&controller, &input, &output);

	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	assert(output.download.rate_bits_per_second == 6U * MEBABIT);
	assert(output.upload.rate_bits_per_second == 6U * MEBABIT);
	assert(output.download.rate_reason == CONTROLLER_RATE_CONGESTION);
	assert(output.upload.rate_reason == CONTROLLER_RATE_CONGESTION);
	controller_close(&controller);
}

/* Three 120 ms deltas: congestion detected with a 60 ms window average. */
static void detect_congestion(
	struct controller *controller,
	struct controller_input *input,
	struct controller_output *output
)
{
	set_latency_delta(input, 120000);
	controller_update(controller, input, output);
	accept_rates(input, output);
	input->timestamp_microseconds += 150001U;
	controller_update(controller, input, output);
	accept_rates(input, output);
	input->timestamp_microseconds += 150001U;
	controller_update(controller, input, output);
}

/* download, upload: expected rates after bufferbloat with one shared delay. */
static void check_queue_attribution(
	bool shared_delay,
	uint64_t download_achieved,
	struct controller_queue_input queue,
	uint64_t expected_download,
	uint64_t expected_upload
)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(download_achieved, 8U * MEBABIT, 7U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.shared_delay = shared_delay;
	input.queue = queue;
	init_controller(&controller, &config);
	detect_congestion(&controller, &input, &output);
	/* A measured split can leave one direction too little delay to detect. */
	if (!queue.valid || !shared_delay) {
		assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
		assert(output.upload.congestion == CONTROLLER_CONGESTION_DETECTED);
	}
	assert(output.download.rate_bits_per_second == expected_download);
	assert(output.upload.rate_bits_per_second == expected_upload);
	controller_close(&controller);
}

static void check_shared_delay_attribution(
	uint64_t download_achieved,
	uint64_t expected_download,
	uint64_t expected_upload
)
{
	const struct controller_queue_input no_queue = { .valid = false };

	check_queue_attribution(
		true,
		download_achieved,
		no_queue,
		expected_download,
		expected_upload
	);
}

static void test_shared_delay_is_attributed_by_download_delivery(void)
{
	/* Download delivers its full shaper rate, so the queue is upload's. */
	check_shared_delay_attribution(8U * MEBABIT, 8U * MEBABIT, 6U * MEBABIT);
	/* Download is loaded but delivers less: it is the bottleneck. */
	check_shared_delay_attribution(7U * MEBABIT, 6U * MEBABIT, 8U * MEBABIT);
	/* An app-limited download says nothing about the queue: both cut. */
	check_shared_delay_attribution(2U * MEBABIT, 6U * MEBABIT, 6U * MEBABIT);
}

static struct controller_queue_input
measured_queue(int64_t download_microseconds, int64_t upload_microseconds)
{
	const struct controller_queue_input queue = {
		.valid = true,
		.download_microseconds = download_microseconds,
		.upload_microseconds = upload_microseconds,
	};

	return queue;
}

static void test_measured_queues_attribute_shared_delay(void)
{
	/*
	 * Download delivers fully, which alone would blame upload. Upload, with no
	 * measured queue, also sees no delay and raises its rate under high load.
	 */
	check_queue_attribution(
		true,
		8U * MEBABIT,
		measured_queue(40000, 0),
		6U * MEBABIT,
		8320U * 1000U
	);
	/* Download delivers less, which alone would blame download. */
	check_queue_attribution(
		true,
		7U * MEBABIT,
		measured_queue(1000, 60000),
		8320U * 1000U,
		6U * MEBABIT
	);
	/* Both hold at least a quarter; download's smaller share cuts less. */
	check_queue_attribution(
		true,
		8U * MEBABIT,
		measured_queue(10000, 30000),
		7920U * 1000U,
		6U * MEBABIT
	);
	/* Less than a quarter is not blamed. */
	check_queue_attribution(
		true,
		7U * MEBABIT,
		measured_queue(9000, 30000),
		8U * MEBABIT,
		6U * MEBABIT
	);
	/* Too little measured queue to explain the delay: delivery decides. */
	check_queue_attribution(
		true,
		8U * MEBABIT,
		measured_queue(4000, 0),
		8U * MEBABIT,
		6U * MEBABIT
	);
	/* Separate one-way delays need no attribution. */
	check_queue_attribution(
		false,
		8U * MEBABIT,
		measured_queue(40000, 0),
		6U * MEBABIT,
		6U * MEBABIT
	);
}

static void test_zero_measured_queues_keep_delivery_fallback(void)
{
	const bool shared_delay = true;
	const struct controller_queue_input zero_queue = measured_queue(0, 0);

	check_queue_attribution(shared_delay, 8U * MEBABIT, zero_queue, 8U * MEBABIT, 6U * MEBABIT);
	check_queue_attribution(shared_delay, 7U * MEBABIT, zero_queue, 6U * MEBABIT, 8U * MEBABIT);
	check_queue_attribution(shared_delay, 2U * MEBABIT, zero_queue, 6U * MEBABIT, 6U * MEBABIT);
}

static void test_measured_queues_split_round_trip_delta(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(8U * MEBABIT, 8U * MEBABIT, 7U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.shared_delay = true;
	init_controller(&controller, &config);
	/* 25 ms each way is below the 30 ms threshold. */
	set_latency_delta(&input, 25000);
	update_repeatedly(&controller, &input, &output, 6U);
	assert(output.download.congestion == CONTROLLER_CONGESTION_CLEAR);
	assert(output.upload.congestion == CONTROLLER_CONGESTION_CLEAR);

	/* The 50 ms round trip splits by the 10/40 ms shares: 10 ms down, 40 ms up. */
	input.queue = measured_queue(10000, 40000);
	update_repeatedly(&controller, &input, &output, 6U);
	assert(output.download.congestion == CONTROLLER_CONGESTION_CLEAR);
	assert(output.upload.congestion == CONTROLLER_CONGESTION_DETECTED);
	assert(output.upload.average_delay_microseconds == 40000);

	/* Below the attribution floor, RTT/2 applies again. */
	input.queue = measured_queue(1000, 3000);
	update_repeatedly(&controller, &input, &output, 6U);
	assert(output.upload.average_delay_microseconds == 25000);
	controller_close(&controller);
}

/* Download after one update, with upload's shaper at 8 Mbit/s. */
static struct controller_direction_output ack_capped_download(
	uint64_t share_percent,
	uint64_t upload_achieved,
	bool acks_valid,
	uint64_t other_rate,
	uint64_t ack_rate
)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(8U * MEBABIT, 8U * MEBABIT, upload_achieved, 8U * MEBABIT);
	struct controller_output output;

	config.ul_congest_ack_share_percent = share_percent;
	input.acks.valid = acks_valid;
	input.acks.upload_ack_rate_bits_per_second = ack_rate;
	input.acks.upload_rate_bits_per_second = other_rate + ack_rate;
	init_controller(&controller, &config);
	/* The first update only writes the base rates. */
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 1000000U;
	controller_update(&controller, &input, &output);
	controller_close(&controller);
	return output.download;
}

#define KBIT(value) ((value) * 1000U)

static void test_ack_share_follows_other_traffic(void)
{
	struct controller_direction_output download;

	/*
	 * Upload saturated (7.9 of 8 Mbit/s). ACKs may use what other traffic
	 * leaves, less 5% (400 kbit/s) headroom: with 100 kbit/s of other
	 * traffic, 7.5 Mbit/s, so download is held at 8 * 7.5 / 7.8.
	 */
	download = ack_capped_download(45U, KBIT(7900U), true, KBIT(100U), KBIT(7800U));
	assert(download.rate_bits_per_second == KBIT(7692U));
	assert(download.rate_reason == CONTROLLER_RATE_ACK_SHARE);
	assert(download.rate_changed);

	/* Other traffic grows to 3 Mbit/s: ACKs get 4.6, download 8 * 4.6 / 4.9. */
	download = ack_capped_download(45U, KBIT(7900U), true, KBIT(3000U), KBIT(4900U));
	assert(download.rate_bits_per_second == KBIT(7510U));

	/* At 4.2 Mbit/s of other traffic ACKs reach their 45% minimum, 3.6. */
	download = ack_capped_download(45U, KBIT(7900U), true, KBIT(4200U), KBIT(3700U));
	assert(download.rate_bits_per_second == KBIT(7783U));

	/* ACKs below their minimum are never held. */
	download = ack_capped_download(45U, KBIT(7900U), true, KBIT(5000U), KBIT(2900U));
	assert(download.rate_bits_per_second == KBIT(8320U));
	assert(download.rate_reason == CONTROLLER_RATE_HIGH_LOAD);

	/* The download minimum still wins (8 * 3.6 / 6 = 4.8 < 5 Mbit/s). */
	download = ack_capped_download(45U, KBIT(7900U), true, KBIT(4200U), KBIT(6000U));
	assert(download.rate_bits_per_second == 5U * MEBABIT);
	assert(download.rate_reason == CONTROLLER_RATE_ACK_SHARE);

	/* No hold without high upload load, without a valid ACK rate, or when off. */
	download = ack_capped_download(45U, KBIT(5000U), true, KBIT(3000U), KBIT(4900U));
	assert(download.rate_bits_per_second == KBIT(8320U));
	download = ack_capped_download(45U, KBIT(7900U), false, KBIT(3000U), KBIT(4900U));
	assert(download.rate_bits_per_second == KBIT(8320U));
	download = ack_capped_download(0U, KBIT(7900U), true, KBIT(3000U), KBIT(4900U));
	assert(download.rate_bits_per_second == KBIT(8320U));
}

static void test_attribution_changes_are_reported(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(8U * MEBABIT, 8U * MEBABIT, 7U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.shared_delay = true;
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	assert(!output.download.bufferbloat_attributed);
	assert(output.download.bufferbloat_attribution_changed);
	assert(output.upload.bufferbloat_attributed);
	assert(!output.upload.bufferbloat_attribution_changed);

	input.queue = measured_queue(40000, 0);
	controller_update(&controller, &input, &output);
	assert(output.download.bufferbloat_attributed &&
	       output.download.bufferbloat_attribution_changed);
	assert(!output.upload.bufferbloat_attributed &&
	       output.upload.bufferbloat_attribution_changed);

	controller_update(&controller, &input, &output);
	assert(!output.download.bufferbloat_attribution_changed);
	assert(!output.upload.bufferbloat_attribution_changed);
	controller_close(&controller);
}

static void test_bufferbloat_reduction_scales_with_average_delay(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	set_latency_delta(&input, 90000);
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 150000U;
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 150001U;
	controller_update(&controller, &input, &output);

	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	assert(output.download.rate_bits_per_second == 6960000U);
	controller_close(&controller);
}

static void test_bufferbloat_reduction_observes_refractory_period(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	set_latency_delta(&input, 120000);
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 150000U;
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 150001U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 6U * MEBABIT);
	accept_rates(&input, &output);

	input.timestamp_microseconds += 299999U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 6U * MEBABIT);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 6U * MEBABIT);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 5U * MEBABIT);
	assert(output.download.rate_reason == CONTROLLER_RATE_CONGESTION);
	controller_close(&controller);
}

static void test_configured_bufferbloat_adjustment_is_used(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.bufferbloat_detection_window = 1U;
	config.bufferbloat_detection_threshold = 1U;
	config.download.delay_threshold_microseconds = 10000U;
	config.download.average_delay_maximum_adjust_down_microseconds = 20000U;
	config.download.minimum_rate_bits_per_second = 1U * MEBABIT;
	config.rate_minimum_adjust_down_bufferbloat_per_thousand = 900U;
	config.rate_maximum_adjust_down_bufferbloat_per_thousand = 500U;
	config.bufferbloat_refractory_period_microseconds = 10U;
	set_latency_delta(&input, 20000);
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 11U;
	controller_update(&controller, &input, &output);

	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	assert(output.download.rate_bits_per_second == 4U * MEBABIT);
	assert(output.download.rate_reason == CONTROLLER_RATE_CONGESTION);
	controller_close(&controller);
}

static void test_low_load_returns_rate_toward_baseline(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 10U * MEBABIT, 1U * MEBABIT, 6U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller.download.initial_rate_pending = false;
	controller.upload.initial_rate_pending = false;
	controller.download.shaper_rate_bits_per_second = 10U * MEBABIT;
	controller.upload.shaper_rate_bits_per_second = 6U * MEBABIT;
	input.timestamp_microseconds = 2000000U;
	controller_update(&controller, &input, &output);

	assert(output.download.rate_bits_per_second == 9900000U);
	assert(output.upload.rate_bits_per_second == 6060000U);
	assert(output.download.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
	assert(output.upload.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
	controller_close(&controller);
}

static void test_low_load_waits_for_decay_refractory_period(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 10U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller.download.initial_rate_pending = false;
	controller.download.shaper_rate_bits_per_second = 10U * MEBABIT;
	controller.download.last_decay_adjustment_microseconds = input.timestamp_microseconds;

	input.timestamp_microseconds += 999999U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 10U * MEBABIT);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 10U * MEBABIT);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 9900000U);
	assert(output.download.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
	controller_close(&controller);
}

static void test_configured_low_load_adjustment_is_used(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 10U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.rate_adjust_down_low_load_per_thousand = 950U;
	config.decay_refractory_period_microseconds = 10U;
	init_controller(&controller, &config);
	controller.download.initial_rate_pending = false;
	controller.download.shaper_rate_bits_per_second = 10U * MEBABIT;
	controller.download.last_decay_adjustment_microseconds = input.timestamp_microseconds;
	input.timestamp_microseconds += 11U;
	controller_update(&controller, &input, &output);

	assert(output.download.rate_bits_per_second == 9500000U);
	assert(output.download.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
	controller_close(&controller);
}

static void test_congestion_restarts_decay_refractory_period(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(1U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;
	uint64_t adjustment_time;
	unsigned int sample;

	set_latency_delta(&input, 120000);
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 150000U;
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 150001U;
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	adjustment_time = input.timestamp_microseconds;

	set_latency_delta(&input, 0);
	for (sample = 0U; sample < 4U; sample++) {
		input.timestamp_microseconds++;
		controller_update(&controller, &input, &output);
	}
	assert(output.download.congestion == CONTROLLER_CONGESTION_CLEAR);

	input.timestamp_microseconds = adjustment_time + 999999U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 6U * MEBABIT);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 6U * MEBABIT);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 6060000U);
	assert(output.download.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
	controller_close(&controller);
}

static void test_high_load_restarts_decay_refractory_period(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(7U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;
	uint64_t adjustment_time;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	input.timestamp_microseconds += 300001U;
	controller_update(&controller, &input, &output);
	accept_rates(&input, &output);
	adjustment_time = input.timestamp_microseconds;

	input.download.traffic_rate_bits_per_second = 1U * MEBABIT;
	input.timestamp_microseconds = adjustment_time + 999999U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8320000U);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8320000U);
	assert(!output.download.rate_changed);

	input.timestamp_microseconds++;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8236000U);
	assert(output.download.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
	controller_close(&controller);
}

static void test_rate_limits_are_hard_bounds(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(9U * MEBABIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.download.maximum_rate_bits_per_second = 8100000U;
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 300001U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 8100000U);

	config.download.minimum_rate_bits_per_second = 7U * MEBABIT;
	config.download.maximum_rate_bits_per_second = 12U * MEBABIT;
	input.download.traffic_rate_bits_per_second = 1U * MEBABIT;
	input.download.cake_rate_bits_per_second = 8U * MEBABIT;
	set_latency_delta(&input, 120000);
	controller_close(&controller);
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 150001U;
	controller_update(&controller, &input, &output);
	input.timestamp_microseconds += 150000U;
	controller_update(&controller, &input, &output);
	assert(output.download.rate_bits_per_second == 7U * MEBABIT);
	controller_close(&controller);
}

static void test_invalid_sample_does_not_adjust_rate(void)
{
	struct controller controller;
	const struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(8U * MEBABIT, 8U * MEBABIT, 8U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	input.download_latency.valid = false;
	input.upload_latency.valid = false;
	controller_update(&controller, &input, &output);

	assert(output.download.rate_bits_per_second == 8U * MEBABIT);
	assert(output.upload.rate_bits_per_second == 8U * MEBABIT);
	assert(!output.download.rate_changed);
	assert(!output.upload.rate_changed);
	controller_close(&controller);
}

static void test_minimum_rate_enforcement_preserves_opt_out(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();

	config.upload.adjust = false;
	init_controller(&controller, &config);
	controller_set_minimum_rates(&controller, 123U);
	assert(controller.download.shaper_rate_bits_per_second ==
	       config.download.minimum_rate_bits_per_second);
	assert(!controller.download.initial_rate_pending);
	assert(controller.download.last_congestion_adjustment_microseconds == 123U);
	assert(controller.download.last_decay_adjustment_microseconds == 123U);
	assert(controller.upload.shaper_rate_bits_per_second ==
	       config.upload.base_rate_bits_per_second);
	controller_close(&controller);
}

static const struct controller_activity_config activity_config = {
	.enable_sleep = true,
	.active_threshold_bits_per_second = 2000000U,
	.stall_threshold_bits_per_second = 10000U,
	.sustained_idle_microseconds = 60000000U,
	.stall_timeout_microseconds = 250000U,
	.global_timeout_microseconds = 10000000U,
};

static struct controller_activity_input activity_input(uint64_t timestamp)
{
	return (struct controller_activity_input){ .download = { .valid = true },
						   .upload = { .valid = true },
						   .timestamp_microseconds = timestamp,
						   .last_response_microseconds = timestamp,
						   .last_pinger_start_microseconds = 1U };
}

static void test_sustained_idle_sleep_and_wakeup(void)
{
	struct controller_activity activity = { .state = CONTROLLER_RUNNING };
	struct controller_activity_input input = activity_input(1U);
	struct controller_activity_output output;

	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.idle_started_microseconds == 1U);
	input = activity_input(60000001U);
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	input = activity_input(60000002U);
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_IDLE);
	assert(output.state_changed);
	input.timestamp_microseconds += 20000000U;
	input.last_response_microseconds = 1U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_IDLE);
	assert(!output.restart_pingers);
	assert(!output.global_timeout_started);
	input.upload.traffic_rate_bits_per_second = 2000000U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_IDLE);
	input.upload.traffic_rate_bits_per_second += 1000U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	assert(output.state_changed);
}

static void test_interrupted_or_invalid_idle_does_not_sleep(void)
{
	struct controller_activity activity = { .state = CONTROLLER_RUNNING };
	struct controller_activity_input input = activity_input(1U);
	struct controller_activity_output output;
	struct controller_activity_config config = activity_config;

	activity_update(&activity, &config, &input, &output);
	input = activity_input(20000000U);
	input.download.traffic_rate_bits_per_second = 2001000U;
	activity_update(&activity, &config, &input, &output);
	assert(activity.idle_started_microseconds == 0U);
	input = activity_input(60000002U);
	activity_update(&activity, &config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	assert(activity.idle_started_microseconds == input.timestamp_microseconds);
	input = activity_input(120000004U);
	input.download.valid = false;
	activity_update(&activity, &config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	assert(activity.idle_started_microseconds == 0U);
	config.enable_sleep = false;
	input = activity_input(200000000U);
	activity_update(&activity, &config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	assert(activity.idle_started_microseconds == 0U);
}

static void test_stall_timeout_restart_and_response_recovery(void)
{
	struct controller_activity activity = { .state = CONTROLLER_RUNNING };
	struct controller_activity_input input = activity_input(1U);
	struct controller_activity_output output;

	input.timestamp_microseconds = 250001U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	input.timestamp_microseconds++;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_STALL);
	assert(output.check_stall_loads);
	assert(output.state_changed);
	assert(!output.global_timeout_started);
	input.timestamp_microseconds = 10000001U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(output.global_timeout_started);
	assert(output.restart_pingers);
	input.last_pinger_start_microseconds = input.timestamp_microseconds;
	input.timestamp_microseconds++;
	activity_update(&activity, &activity_config, &input, &output);
	assert(!output.global_timeout_started);
	assert(!output.restart_pingers);
	input.last_response_microseconds = input.timestamp_microseconds;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	assert(output.state_changed);
	assert(!activity.global_timeout_reported);
}

static void test_both_loads_bypass_stall_but_not_global_timeout(void)
{
	struct controller_activity activity = { .state = CONTROLLER_RUNNING };
	struct controller_activity_input input = activity_input(1U);
	struct controller_activity_output output;

	input.timestamp_microseconds = 10000001U;
	input.download.traffic_rate_bits_per_second = 11000U;
	input.upload.traffic_rate_bits_per_second = 11000U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	assert(output.global_timeout_started);
	assert(output.restart_pingers);
	input.upload.traffic_rate_bits_per_second = 10000U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_STALL);
	input.upload.traffic_rate_bits_per_second += 1000U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	assert(output.state_changed);
}

static void test_wakeup_grace_prevents_false_stall(void)
{
	struct controller_activity activity = { .state = CONTROLLER_RUNNING };
	struct controller_activity_input input = activity_input(1U);
	struct controller_activity_output output;

	input.timestamp_microseconds = 600000U;
	input.grace_until_microseconds = 600001U;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_RUNNING);
	assert(!output.check_stall_loads);
	input.timestamp_microseconds++;
	activity_update(&activity, &activity_config, &input, &output);
	assert(activity.state == CONTROLLER_STALL);
}

static void test_compact_delay_window_boundaries(void)
{
	struct controller controller;
	struct controller_config config = default_config();
	struct controller_input input = input_with_rates(0U, 1U, 0U, 1U);
	struct controller_output output;

	config.bufferbloat_detection_window = 1U;
	config.bufferbloat_detection_threshold = 1U;
	config.download.delay_threshold_microseconds = 0U;
	config.upload.delay_threshold_microseconds = UINT64_MAX;
	init_controller(&controller, &config);
	set_latency_delta(&input, INT64_C(2147483647));
	controller_update(&controller, &input, &output);
	assert(output.download.average_delay_microseconds == INT32_MAX);
	assert(output.download.delayed_sample_count == 1U);
	assert(output.upload.delayed_sample_count == 0U);

	set_latency_delta(&input, -INT64_C(2147483647));
	controller_update(&controller, &input, &output);
	assert(output.download.average_delay_microseconds == -INT64_C(2147483647));
	assert(output.download.delayed_sample_count == 0U);
	assert(output.upload.delayed_sample_count == 0U);

	set_latency_delta(&input, 0);
	controller_update(&controller, &input, &output);
	assert(controller.download.delay_sum_microseconds == 0);
	assert(output.download.delayed_sample_count == 0U);
	controller_close(&controller);
}

static void test_directional_latency_windows_are_independent(void)
{
	struct controller controller;
	struct controller_config config = default_config();
	struct controller_input input = input_with_rates(0U, 1U, 0U, 1U);
	struct controller_output output;

	config.bufferbloat_detection_window = 3U;
	config.bufferbloat_detection_threshold = 2U;
	init_controller(&controller, &config);
	input.download_latency.owd_delta_microseconds = 40000;
	input.upload_latency.owd_delta_microseconds = -5000;
	update_repeatedly(&controller, &input, &output, 2U);

	assert(controller.download.delay_sum_microseconds == 80000);
	assert(output.download.average_delay_microseconds == 26666);
	assert(output.download.delayed_sample_count == 2U);
	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	assert(controller.upload.delay_sum_microseconds == -10000);
	assert(output.upload.average_delay_microseconds == -3333);
	assert(output.upload.delayed_sample_count == 0U);
	assert(output.upload.congestion == CONTROLLER_CONGESTION_CLEAR);

	input.download_latency.valid = false;
	input.upload_latency.owd_delta_microseconds = 40000;
	controller_update(&controller, &input, &output);
	assert(output.download.congestion == CONTROLLER_CONGESTION_UNKNOWN);
	assert(controller.download.delay_sum_microseconds == 80000);
	assert(output.download.delayed_sample_count == 2U);
	assert(controller.upload.delay_sum_microseconds == 30000);
	assert(output.upload.delayed_sample_count == 1U);

	input.download_latency.valid = true;
	input.download_latency.owd_delta_microseconds = -1;
	controller.download.config.delay_threshold_microseconds = UINT64_MAX;
	controller_update(&controller, &input, &output);
	assert(controller.download.delay_sum_microseconds == 79999);
	assert(output.download.delayed_sample_count == 2U);
	assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
	controller_close(&controller);
}

static void test_delay_window_matches_rescanned_history(void)
{
	enum {
		WINDOW_SIZE = 17,
		SAMPLE_COUNT = 4096
	};
	struct controller controller;
	struct controller_config config = default_config();
	struct controller_input input = input_with_rates(0U, 1U, 0U, 1U);
	struct controller_output output;
	int64_t history[WINDOW_SIZE] = { 0 };
	uint32_t random = 1U;

	config.bufferbloat_detection_window = WINDOW_SIZE;
	config.bufferbloat_detection_threshold = 5U;
	config.download.delay_threshold_microseconds = 30000U;
	config.upload.delay_threshold_microseconds = 1000000000U;
	init_controller(&controller, &config);
	for (unsigned int index = 0U; index < SAMPLE_COUNT; index++) {
		int64_t sum = 0;
		unsigned int download_delays = 0U;
		unsigned int upload_delays = 0U;

		random = random * 1664525U + 1013904223U;
		{
			uint32_t current = random;

			random = random * 1664525U + 1013904223U;
			history[index % WINDOW_SIZE] = ((int64_t)current - (int64_t)random) / 2;
			set_latency_delta(&input, history[index % WINDOW_SIZE]);
		}
		for (unsigned int slot = 0U; slot < WINDOW_SIZE; slot++) {
			sum += history[slot];
			download_delays += history[slot] > 30000 ? 1U : 0U;
			upload_delays += history[slot] > 1000000000 ? 1U : 0U;
		}
		controller_update(&controller, &input, &output);
		assert(controller.download.delay_sum_microseconds == sum);
		assert(controller.upload.delay_sum_microseconds == sum);
		assert(output.download.average_delay_microseconds == sum / WINDOW_SIZE);
		assert(output.upload.average_delay_microseconds == sum / WINDOW_SIZE);
		assert(output.download.delayed_sample_count == download_delays);
		assert(output.upload.delayed_sample_count == upload_delays);
	}
	controller_close(&controller);
}

static void test_compensation_saturates_thresholds(void)
{
	struct controller controller;
	struct controller_config config = default_config();

	config.download.delay_threshold_microseconds = UINT64_MAX - 1U;
	init_controller(&controller, &config);
	controller_set_serialization_compensation(&controller, 12000U, 0U, 1U, 1U);
	assert(controller.download.config.delay_threshold_microseconds == UINT64_MAX);
	controller_close(&controller);
}

static void test_compensation_is_directional_and_preserves_history(void)
{
	struct controller controller;
	struct controller_config config = default_config();
	struct controller_input input = input_with_rates(1U, 1U, 1U, 1U);
	struct controller_output output;

	config.upload.average_delay_maximum_adjust_up_microseconds = 11000U;
	config.upload.delay_threshold_microseconds = 31000U;
	config.upload.average_delay_maximum_adjust_down_microseconds = 61000U;
	input.download_latency.owd_delta_microseconds = 30001;
	input.upload_latency.owd_delta_microseconds = 31001;
	init_controller(&controller, &config);
	controller_update(&controller, &input, &output);
	assert(controller.download.delayed_sample_count == 1U);
	assert(controller.upload.delayed_sample_count == 1U);

	controller_set_serialization_compensation(&controller, 12000U, 24000U, 1000000U, 2000000U);
	assert(controller.config.download.delay_threshold_microseconds == 30000U);
	assert(controller.config.upload.delay_threshold_microseconds == 31000U);
	assert(controller.download.config.average_delay_maximum_adjust_up_microseconds == 22000U);
	assert(controller.download.config.delay_threshold_microseconds == 42000U);
	assert(controller.download.config.average_delay_maximum_adjust_down_microseconds == 72000U);
	assert(controller.upload.config.average_delay_maximum_adjust_up_microseconds == 23000U);
	assert(controller.upload.config.delay_threshold_microseconds == 43000U);
	assert(controller.upload.config.average_delay_maximum_adjust_down_microseconds == 73000U);
	assert(controller.download.delayed_sample_count == 1U);
	assert(controller.upload.delayed_sample_count == 1U);

	controller_set_serialization_compensation(&controller, 6000U, 6000U, 1000000U, 3000000U);
	assert(controller.download.config.average_delay_maximum_adjust_up_microseconds == 16000U);
	assert(controller.download.config.delay_threshold_microseconds == 36000U);
	assert(controller.download.config.average_delay_maximum_adjust_down_microseconds == 66000U);
	assert(controller.upload.config.average_delay_maximum_adjust_up_microseconds == 13000U);
	assert(controller.upload.config.delay_threshold_microseconds == 33000U);
	assert(controller.upload.config.average_delay_maximum_adjust_down_microseconds == 63000U);
	assert(controller.download.delayed_sample_count == 1U);
	assert(controller.upload.delayed_sample_count == 1U);
	controller_close(&controller);
}

static void test_load_classification(void)
{
	struct controller controller;
	const struct controller_config config = default_config();
	struct controller_direction_input input = {
		.valid = true,
		.traffic_rate_bits_per_second = 0U,
		.cake_rate_bits_per_second = 8U * MEBABIT,
	};
	struct controller_direction_input other = input;

	init_controller(&controller, &config);
	/* 75% is not above the 75% threshold; 76% is. */
	input.traffic_rate_bits_per_second = 6U * MEBABIT;
	assert(controller_load(&controller, &input, 2U * MEBABIT) == CONTROLLER_LOAD_LOW);
	input.traffic_rate_bits_per_second = 6080U * 1000U;
	assert(controller_load(&controller, &input, 2U * MEBABIT) == CONTROLLER_LOAD_HIGH);
	/* Traffic must exceed the active threshold to be low rather than idle. */
	input.traffic_rate_bits_per_second = 2U * MEBABIT;
	assert(controller_load(&controller, &input, 2U * MEBABIT) == CONTROLLER_LOAD_IDLE);
	input.traffic_rate_bits_per_second = 2001U * 1000U;
	assert(controller_load(&controller, &input, 2U * MEBABIT) == CONTROLLER_LOAD_LOW);

	/* Low load needs both directions strictly below the threshold. */
	input.traffic_rate_bits_per_second = 5920U * 1000U;
	other.traffic_rate_bits_per_second = 1U * MEBABIT;
	assert(controller_low_load(&controller, &input, &other));
	input.traffic_rate_bits_per_second = 6U * MEBABIT;
	assert(!controller_low_load(&controller, &input, &other));
	assert(!controller_low_load(&controller, &other, &input));
	/* Without a CAKE rate the load counts as zero, like cake-autorate's first sample. */
	input.cake_rate_bits_per_second = 0U;
	assert(controller_low_load(&controller, &input, &other));
	controller_close(&controller);
}

int main(void)
{
	test_initial_state_is_unknown();
	test_load_classification();
	test_allocation_failure_releases_every_window();
	test_compensation_saturates_thresholds();
	test_compensation_is_directional_and_preserves_history();
	test_compact_delay_window_boundaries();
	test_delay_window_matches_rescanned_history();
	test_low_load_is_below_capacity();
	test_sustained_download_is_saturated();
	test_brief_burst_does_not_saturate();
	test_line_hysteresis_and_recovery();
	test_invalid_direction_returns_to_unknown();
	test_directional_latency_windows_are_independent();
	test_three_of_six_delays_detect_bufferbloat();
	test_below_baseline_delay_remains_signed();
	test_delay_window_clears_after_old_delays_expire();
	test_missing_probe_holds_delay_window();
	test_configured_delay_window_and_direction_thresholds();
	test_invalid_delay_window_is_rejected();
	test_initial_rate_is_baseline();
	test_initial_rate_is_written_even_when_cake_holds_it();
	test_initial_rate_waits_for_valid_qdisc_input();
	test_high_load_increases_rate_four_percent();
	test_high_load_consumes_each_direction_sample_once();
	test_invalid_input_preserves_fresh_sample();
	test_noop_increase_consumes_sample();
	test_high_load_waits_for_congestion_refractory_period();
	test_configured_high_load_adjustment_is_used();
	test_shared_delay_is_attributed_by_download_delivery();
	test_measured_queues_attribute_shared_delay();
	test_zero_measured_queues_keep_delivery_fallback();
	test_measured_queues_split_round_trip_delta();
	test_ack_share_follows_other_traffic();
	test_attribution_changes_are_reported();
	test_severe_bufferbloat_reduces_both_rates();
	test_bufferbloat_reduction_scales_with_average_delay();
	test_bufferbloat_reduction_observes_refractory_period();
	test_configured_bufferbloat_adjustment_is_used();
	test_low_load_returns_rate_toward_baseline();
	test_low_load_waits_for_decay_refractory_period();
	test_configured_low_load_adjustment_is_used();
	test_congestion_restarts_decay_refractory_period();
	test_high_load_restarts_decay_refractory_period();
	test_rate_limits_are_hard_bounds();
	test_invalid_sample_does_not_adjust_rate();
	test_minimum_rate_enforcement_preserves_opt_out();
	test_sustained_idle_sleep_and_wakeup();
	test_interrupted_or_invalid_idle_does_not_sleep();
	test_stall_timeout_restart_and_response_recovery();
	test_both_loads_bypass_stall_but_not_global_timeout();
	test_wakeup_grace_prevents_false_stall();

	(void)puts("controller tests passed");
	return 0;
}
