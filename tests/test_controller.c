#include "controller.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define MEBABIT 1000000U

static struct controller_config monitor_config(void)
{
    return (struct controller_config) { 0 };
}

static struct controller_config adjusting_config(void)
{
    return (struct controller_config) {
        .download = {
            .adjust = true,
            .minimum_rate_bits_per_second = 5U * MEBABIT,
            .base_rate_bits_per_second = 8U * MEBABIT,
            .maximum_rate_bits_per_second = 12U * MEBABIT
        },
        .upload = {
            .adjust = true,
            .minimum_rate_bits_per_second = 5U * MEBABIT,
            .base_rate_bits_per_second = 8U * MEBABIT,
            .maximum_rate_bits_per_second = 12U * MEBABIT
        }
    };
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
            .traffic_rate_bits_per_second = download_rate,
            .cake_rate_bits_per_second = download_limit
        },
        .upload = {
            .valid = true,
            .traffic_rate_bits_per_second = upload_rate,
            .cake_rate_bits_per_second = upload_limit
        },
        .latency = {
            .valid = true,
            .current_rtt_microseconds = 30000U,
            .baseline_rtt_microseconds = 30000U
        },
        .timestamp_microseconds = 1000001U
    };
}

static void update_repeatedly(
    struct sqm_mon_controller *controller,
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

static void test_initial_state_is_unknown(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();

    controller_init(&controller, &config);

    assert(controller.download.state == CONTROLLER_LINE_UNKNOWN);
    assert(controller.upload.state == CONTROLLER_LINE_UNKNOWN);
    assert(controller.download.congestion == CONTROLLER_CONGESTION_UNKNOWN);
    assert(controller.upload.congestion == CONTROLLER_CONGESTION_UNKNOWN);
}

static void test_low_load_is_below_capacity(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);

    assert(output.download.state == CONTROLLER_LINE_BELOW_CAPACITY);
    assert(output.upload.state == CONTROLLER_LINE_BELOW_CAPACITY);
    assert(output.download.congestion == CONTROLLER_CONGESTION_CLEAR);
    assert(output.upload.congestion == CONTROLLER_CONGESTION_CLEAR);
    assert(!output.download.rate_changed);
    assert(!output.upload.rate_changed);
}

static void test_sustained_download_is_saturated(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
    struct controller_input input = input_with_rates(
        7200000U,
        8U * MEBABIT,
        100000U,
        8U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    assert(output.download.state == CONTROLLER_LINE_UNKNOWN);
    controller_update(&controller, &input, &output);
    assert(output.download.state == CONTROLLER_LINE_UNKNOWN);
    controller_update(&controller, &input, &output);

    assert(output.download.state == CONTROLLER_LINE_SATURATED);
    assert(output.download.state_changed);
    assert(output.upload.state == CONTROLLER_LINE_BELOW_CAPACITY);
}

static void test_brief_burst_does_not_saturate(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
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

    controller_init(&controller, &config);
    update_repeatedly(&controller, &high, &output, 2U);
    controller_update(&controller, &low, &output);

    assert(output.download.state == CONTROLLER_LINE_BELOW_CAPACITY);
}

static void test_line_hysteresis_and_recovery(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
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

    controller_init(&controller, &config);
    update_repeatedly(&controller, &high, &output, 3U);
    update_repeatedly(&controller, &middle, &output, 5U);
    assert(output.download.state == CONTROLLER_LINE_SATURATED);

    update_repeatedly(&controller, &low, &output, 2U);
    assert(output.download.state == CONTROLLER_LINE_SATURATED);
    controller_update(&controller, &low, &output);
    assert(output.download.state == CONTROLLER_LINE_BELOW_CAPACITY);
}

static void test_invalid_direction_returns_to_unknown(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    input.download.valid = false;
    controller_update(&controller, &input, &output);

    assert(output.download.state == CONTROLLER_LINE_UNKNOWN);
    assert(output.download.state_changed);
}

static void test_three_of_six_delays_detect_bufferbloat(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 90002U;
    controller_init(&controller, &config);
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
}

static void test_below_baseline_delay_remains_signed(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    input.latency.baseline_rtt_microseconds = 30000U;
    input.latency.current_rtt_microseconds = 24000U;
    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);

    assert(output.download.delay_sum_microseconds == -3000);
    assert(output.upload.delay_sum_microseconds == -3000);
    assert(output.download.average_delay_microseconds == -500);
    assert(output.upload.average_delay_microseconds == -500);
    assert(output.download.delayed_sample_count == 0U);
    assert(output.upload.delayed_sample_count == 0U);
}

static void test_delay_window_clears_after_old_delays_expire(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 90002U;
    controller_init(&controller, &config);
    update_repeatedly(&controller, &input, &output, 3U);
    input.latency.current_rtt_microseconds = 30000U;
    update_repeatedly(&controller, &input, &output, 3U);
    assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
    controller_update(&controller, &input, &output);

    assert(output.download.congestion == CONTROLLER_CONGESTION_CLEAR);
    assert(output.download.congestion_changed);
}

static void test_missing_probe_holds_delay_window(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = monitor_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 90002U;
    controller_init(&controller, &config);
    update_repeatedly(&controller, &input, &output, 2U);
    input.latency.valid = false;
    controller_update(&controller, &input, &output);
    assert(output.download.congestion == CONTROLLER_CONGESTION_UNKNOWN);
    input.latency.valid = true;
    controller_update(&controller, &input, &output);

    assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
}

static void test_initial_rate_is_baseline(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        6U * MEBABIT,
        1U * MEBABIT,
        6U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);

    assert(output.download.rate_bits_per_second == 8U * MEBABIT);
    assert(output.upload.rate_bits_per_second == 8U * MEBABIT);
    assert(output.download.rate_changed);
    assert(output.download.rate_reason == CONTROLLER_RATE_INITIAL);
}

static void test_initial_rate_waits_for_valid_qdisc_input(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        6U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    input.download.valid = false;
    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    assert(!output.download.rate_changed);
    assert(controller.download.initial_rate_pending);

    input.download.valid = true;
    controller_update(&controller, &input, &output);
    assert(output.download.rate_bits_per_second == 8U * MEBABIT);
    assert(output.download.rate_changed);
    assert(output.download.rate_reason == CONTROLLER_RATE_INITIAL);
}

static void test_high_load_increases_rate_four_percent(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        6000001U,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    input.timestamp_microseconds += 300000U;
    controller_update(&controller, &input, &output);

    assert(output.download.rate_bits_per_second == 8320000U);
    assert(output.download.rate_changed);
    assert(output.download.rate_reason == CONTROLLER_RATE_HIGH_LOAD);
}

static void test_high_load_waits_for_congestion_refractory_period(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        6000001U,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    input.timestamp_microseconds += 299999U;
    controller_update(&controller, &input, &output);
    assert(output.download.rate_bits_per_second == 8U * MEBABIT);
    assert(!output.download.rate_changed);

    input.timestamp_microseconds++;
    controller_update(&controller, &input, &output);
    assert(output.download.rate_bits_per_second == 8320000U);
    assert(output.download.rate_reason == CONTROLLER_RATE_HIGH_LOAD);
}

static void test_severe_bufferbloat_reduces_both_rates(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 270000U;
    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);

    assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
    assert(output.download.rate_bits_per_second == 6U * MEBABIT);
    assert(output.upload.rate_bits_per_second == 6U * MEBABIT);
    assert(output.download.rate_reason == CONTROLLER_RATE_CONGESTION);
    assert(output.upload.rate_reason == CONTROLLER_RATE_CONGESTION);
}

static void test_bufferbloat_reduction_scales_with_average_delay(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 210000U;
    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);

    assert(output.download.congestion == CONTROLLER_CONGESTION_DETECTED);
    assert(output.download.rate_bits_per_second == 6960000U);
}

static void test_bufferbloat_reduction_observes_refractory_period(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 270000U;
    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);
    assert(output.download.rate_bits_per_second == 6U * MEBABIT);
    accept_rates(&input, &output);

    input.timestamp_microseconds += 299999U;
    controller_update(&controller, &input, &output);
    assert(output.download.rate_bits_per_second == 6U * MEBABIT);
    assert(!output.download.rate_changed);

    input.timestamp_microseconds++;
    controller_update(&controller, &input, &output);
    assert(output.download.rate_bits_per_second == 5U * MEBABIT);
    assert(output.download.rate_reason == CONTROLLER_RATE_CONGESTION);
}

static void test_low_load_returns_rate_toward_baseline(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        10U * MEBABIT,
        1U * MEBABIT,
        6U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
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
}

static void test_low_load_waits_for_decay_refractory_period(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        1U * MEBABIT,
        10U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
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
    assert(output.download.rate_bits_per_second == 9900000U);
    assert(output.download.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
}

static void test_congestion_restarts_decay_refractory_period(void)
{
    struct sqm_mon_controller controller;
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

    input.latency.current_rtt_microseconds = 270000U;
    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    adjustment_time = input.timestamp_microseconds;

    input.latency.current_rtt_microseconds = 30000U;
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
    assert(output.download.rate_bits_per_second == 6060000U);
    assert(output.download.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
}

static void test_high_load_restarts_decay_refractory_period(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        7U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;
    uint64_t adjustment_time;

    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    accept_rates(&input, &output);
    input.timestamp_microseconds += 300000U;
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
    assert(output.download.rate_bits_per_second == 8236800U);
    assert(output.download.rate_reason == CONTROLLER_RATE_RETURN_TO_BASE);
}

static void test_rate_limits_are_hard_bounds(void)
{
    struct sqm_mon_controller controller;
    struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        9U * MEBABIT,
        8U * MEBABIT,
        1U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    config.download.maximum_rate_bits_per_second = 8100000U;
    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    input.timestamp_microseconds += 300000U;
    controller_update(&controller, &input, &output);
    assert(output.download.rate_bits_per_second == 8100000U);

    config.download.minimum_rate_bits_per_second = 7U * MEBABIT;
    config.download.maximum_rate_bits_per_second = 12U * MEBABIT;
    input.download.traffic_rate_bits_per_second = 1U * MEBABIT;
    input.download.cake_rate_bits_per_second = 8U * MEBABIT;
    input.latency.current_rtt_microseconds = 270000U;
    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);
    input.timestamp_microseconds += 150000U;
    controller_update(&controller, &input, &output);
    assert(output.download.rate_bits_per_second == 7U * MEBABIT);
}

static void test_invalid_sample_does_not_adjust_rate(void)
{
    struct sqm_mon_controller controller;
    const struct controller_config config = adjusting_config();
    struct controller_input input = input_with_rates(
        8U * MEBABIT,
        8U * MEBABIT,
        8U * MEBABIT,
        8U * MEBABIT
    );
    struct controller_output output;

    controller_init(&controller, &config);
    controller_update(&controller, &input, &output);
    input.latency.valid = false;
    controller_update(&controller, &input, &output);

    assert(output.download.rate_bits_per_second == 8U * MEBABIT);
    assert(output.upload.rate_bits_per_second == 8U * MEBABIT);
    assert(!output.download.rate_changed);
    assert(!output.upload.rate_changed);
}

int main(void)
{
    test_initial_state_is_unknown();
    test_low_load_is_below_capacity();
    test_sustained_download_is_saturated();
    test_brief_burst_does_not_saturate();
    test_line_hysteresis_and_recovery();
    test_invalid_direction_returns_to_unknown();
    test_three_of_six_delays_detect_bufferbloat();
    test_below_baseline_delay_remains_signed();
    test_delay_window_clears_after_old_delays_expire();
    test_missing_probe_holds_delay_window();
    test_initial_rate_is_baseline();
    test_initial_rate_waits_for_valid_qdisc_input();
    test_high_load_increases_rate_four_percent();
    test_high_load_waits_for_congestion_refractory_period();
    test_severe_bufferbloat_reduces_both_rates();
    test_bufferbloat_reduction_scales_with_average_delay();
    test_bufferbloat_reduction_observes_refractory_period();
    test_low_load_returns_rate_toward_baseline();
    test_low_load_waits_for_decay_refractory_period();
    test_congestion_restarts_decay_refractory_period();
    test_high_load_restarts_decay_refractory_period();
    test_rate_limits_are_hard_bounds();
    test_invalid_sample_does_not_adjust_rate();

    (void)puts("controller tests passed");
    return 0;
}
