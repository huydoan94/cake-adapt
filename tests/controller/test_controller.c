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
    if (failing_allocation != 0U && ++allocations == failing_allocation) {
        return NULL;
    }
    return __real_calloc(count, size);
}

static struct controller_config default_config(void)
{
    return (struct controller_config) {
        .download = {
            .average_delay_maximum_adjust_up_microseconds = 10000U,
            .delay_threshold_microseconds = 30000U,
            .average_delay_maximum_adjust_down_microseconds = 60000U
        },
        .upload = {
            .average_delay_maximum_adjust_up_microseconds = 10000U,
            .delay_threshold_microseconds = 30000U,
            .average_delay_maximum_adjust_down_microseconds = 60000U
        },
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
    return (struct controller_input) {
        .download = {
            .valid = true,
            .traffic_sample_id = 1U,
            .traffic_rate_bits_per_second = download_rate,
            .cake_rate_bits_per_second = download_limit
        },
        .upload = {
            .valid = true,
            .traffic_sample_id = 1U,
            .traffic_rate_bits_per_second = upload_rate,
            .cake_rate_bits_per_second = upload_limit
        },
        .download_latency = { .valid = true, .owd_delta_microseconds = 0 },
        .upload_latency = { .valid = true, .owd_delta_microseconds = 0 },
        .timestamp_microseconds = 1000001U
    };
}

static void set_latency_delta(
    struct controller_input *input,
    int64_t delta_microseconds
)
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

    for (index = 0U; index < count; index++) {
        controller_update(controller, input, output);
    }
}

static void accept_rates(
    struct controller_input *input,
    const struct controller_output *output
)
{
    input->download.cake_rate_bits_per_second =
        output->download.rate_bits_per_second;
    input->upload.cake_rate_bits_per_second =
        output->upload.rate_bits_per_second;
}

static void init_controller(
    struct controller *controller,
    const struct controller_config *config
)
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        7200000U,
        8U * MEBABIT,
        100000U,
        8U * MEBABIT
    );
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
    struct controller_input high = input_with_rates(
        8U * MEBABIT,
        8U * MEBABIT,
        0U,
        8U * MEBABIT
    );
    struct controller_input low = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        0U,
        8U * MEBABIT
    );
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
    struct controller_input high = input_with_rates(
        8U * MEBABIT,
        8U * MEBABIT,
        0U,
        8U * MEBABIT
    );
    struct controller_input middle = input_with_rates(
        6800000U,
        8U * MEBABIT,
        0U,
        8U * MEBABIT
    );
    struct controller_input low = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        0U,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    assert(output.download.delay_sum_microseconds == 90003U);
    assert(output.upload.delay_sum_microseconds == 90003U);
    assert(output.download.average_delay_microseconds == 15000U);
    assert(output.upload.average_delay_microseconds == 15000U);
    controller_close(&controller);
}

static void test_below_baseline_delay_remains_signed(void)
{
    struct controller controller;
    const struct controller_config config = default_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    set_latency_delta(&input, -3000);
    init_controller(&controller, &config);
    controller_update(&controller, &input, &output);

    assert(output.download.delay_sum_microseconds == -3000);
    assert(output.upload.delay_sum_microseconds == -3000);
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        6U * MEBABIT,
        1U * MEBABIT,
        6U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        6U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        6080000U,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        20U * MEBABIT,
        8U * MEBABIT,
        20U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        20U * MEBABIT,
        8U * MEBABIT,
        20U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        20U * MEBABIT,
        8U * MEBABIT,
        20U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        6080000U,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        4080000U,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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

static void test_bufferbloat_reduction_scales_with_average_delay(void)
{
    struct controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        10U * MEBABIT,
        1U * MEBABIT,
        6U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        10U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    init_controller(&controller, &config);
    controller.download.initial_rate_pending = false;
    controller.download.shaper_rate_bits_per_second = 10U * MEBABIT;
    controller.download.last_decay_adjustment_microseconds =
        input.timestamp_microseconds;

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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        10U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    config.rate_adjust_down_low_load_per_thousand = 950U;
    config.decay_refractory_period_microseconds = 10U;
    init_controller(&controller, &config);
    controller.download.initial_rate_pending = false;
    controller.download.shaper_rate_bits_per_second = 10U * MEBABIT;
    controller.download.last_decay_adjustment_microseconds =
        input.timestamp_microseconds;
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
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        7U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        9U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
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
    struct controller_input input = input_with_rates(
        8U * MEBABIT,
        8U * MEBABIT,
        8U * MEBABIT,
        8U * MEBABIT
    );
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
    assert(controller.download.shaper_rate_bits_per_second == config.download.minimum_rate_bits_per_second);
    assert(!controller.download.initial_rate_pending);
    assert(controller.download.last_congestion_adjustment_microseconds == 123U);
    assert(controller.download.last_decay_adjustment_microseconds == 123U);
    assert(controller.upload.shaper_rate_bits_per_second == config.upload.base_rate_bits_per_second);
    controller_close(&controller);
}

static const struct controller_activity_config activity_config = {
    .enable_sleep = true,
    .active_threshold_bits_per_second = 2000000U,
    .stall_threshold_bits_per_second = 10000U,
    .sustained_idle_microseconds = 60000000U,
    .stall_timeout_microseconds = 250000U,
    .global_timeout_microseconds = 10000000U
};

static struct controller_activity_input activity_input(uint64_t timestamp)
{
    return (struct controller_activity_input) {
        .download = { .valid = true },
        .upload = { .valid = true },
        .timestamp_microseconds = timestamp,
        .last_response_microseconds = timestamp,
        .last_pinger_start_microseconds = 1U
    };
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
    assert(output.download.delay_sum_microseconds == 0);
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

    assert(output.download.delay_sum_microseconds == 80000);
    assert(output.download.average_delay_microseconds == 26666);
    assert(output.download.delayed_sample_count == 2U);
    assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
    assert(output.upload.delay_sum_microseconds == -10000);
    assert(output.upload.average_delay_microseconds == -3333);
    assert(output.upload.delayed_sample_count == 0U);
    assert(output.upload.congestion == CONTROLLER_CONGESTION_CLEAR);

    input.download_latency.valid = false;
    input.upload_latency.owd_delta_microseconds = 40000;
    controller_update(&controller, &input, &output);
    assert(output.download.congestion == CONTROLLER_CONGESTION_UNKNOWN);
    assert(output.download.delay_sum_microseconds == 80000);
    assert(output.download.delayed_sample_count == 2U);
    assert(output.upload.delay_sum_microseconds == 30000);
    assert(output.upload.delayed_sample_count == 1U);

    input.download_latency.valid = true;
    input.download_latency.owd_delta_microseconds = -1;
    controller.download.config.delay_threshold_microseconds = UINT64_MAX;
    controller_update(&controller, &input, &output);
    assert(output.download.delay_sum_microseconds == 79999);
    assert(output.download.delayed_sample_count == 2U);
    assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
    controller_close(&controller);
}

static void test_delay_window_matches_rescanned_history(void)
{
    enum { WINDOW_SIZE = 17, SAMPLE_COUNT = 4096 };
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
        assert(output.download.delay_sum_microseconds == sum);
        assert(output.upload.delay_sum_microseconds == sum);
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
    controller_set_serialization_compensation(
        &controller,
        12000U,
        0U,
        1U,
        1U
    );
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

    controller_set_serialization_compensation(
        &controller,
        12000U,
        24000U,
        1000000U,
        2000000U
    );
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

    controller_set_serialization_compensation(
        &controller,
        6000U,
        6000U,
        1000000U,
        3000000U
    );
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

int main(void)
{
    test_initial_state_is_unknown();
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
