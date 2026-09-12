#include "controller.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

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
        }
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

static void test_initial_state_is_unknown(void)
{
    struct sqm_mon_controller controller;

    controller_init(&controller);

    assert(controller.download.state == CONTROLLER_LINE_UNKNOWN);
    assert(controller.upload.state == CONTROLLER_LINE_UNKNOWN);
    assert(controller.download.congestion == CONTROLLER_CONGESTION_UNKNOWN);
    assert(controller.upload.congestion == CONTROLLER_CONGESTION_UNKNOWN);
}

static void test_low_load_is_below_capacity(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        1000000U,
        8000000U,
        1000000U,
        8000000U
    );
    struct controller_output output;

    controller_init(&controller);
    controller_update(&controller, &input, &output);

    assert(output.download_state == CONTROLLER_LINE_BELOW_CAPACITY);
    assert(output.upload_state == CONTROLLER_LINE_BELOW_CAPACITY);
    assert(output.download_state_changed);
    assert(output.upload_state_changed);
    assert(output.download_congestion == CONTROLLER_CONGESTION_CLEAR);
    assert(output.upload_congestion == CONTROLLER_CONGESTION_CLEAR);
}

static void test_sustained_download_is_saturated(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        7200000U,
        8000000U,
        100000U,
        8000000U
    );
    struct controller_output output;

    controller_init(&controller);
    controller_update(&controller, &input, &output);
    assert(output.download_state == CONTROLLER_LINE_UNKNOWN);
    controller_update(&controller, &input, &output);
    assert(output.download_state == CONTROLLER_LINE_UNKNOWN);
    controller_update(&controller, &input, &output);

    assert(output.download_state == CONTROLLER_LINE_SATURATED);
    assert(output.download_state_changed);
    assert(output.upload_state == CONTROLLER_LINE_BELOW_CAPACITY);
    assert(output.download_congestion == CONTROLLER_CONGESTION_CLEAR);
}

static void test_brief_burst_does_not_saturate(void)
{
    struct sqm_mon_controller controller;
    struct controller_input high = input_with_rates(
        8000000U,
        8000000U,
        0U,
        8000000U
    );
    struct controller_input low = input_with_rates(
        1000000U,
        8000000U,
        0U,
        8000000U
    );
    struct controller_output output;

    controller_init(&controller);
    update_repeatedly(&controller, &high, &output, 2U);
    controller_update(&controller, &low, &output);

    assert(output.download_state == CONTROLLER_LINE_BELOW_CAPACITY);
}

static void test_upload_is_independent(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        100000U,
        8000000U,
        9000000U,
        10000000U
    );
    struct controller_output output;

    controller_init(&controller);
    update_repeatedly(&controller, &input, &output, 3U);

    assert(output.download_state == CONTROLLER_LINE_BELOW_CAPACITY);
    assert(output.upload_state == CONTROLLER_LINE_SATURATED);
}

static void test_hysteresis_prevents_flapping(void)
{
    struct sqm_mon_controller controller;
    struct controller_input high = input_with_rates(
        8000000U,
        8000000U,
        0U,
        8000000U
    );
    struct controller_input middle = input_with_rates(
        6800000U,
        8000000U,
        0U,
        8000000U
    );
    struct controller_output output;

    controller_init(&controller);
    update_repeatedly(&controller, &high, &output, 3U);
    update_repeatedly(&controller, &middle, &output, 5U);

    assert(output.download_state == CONTROLLER_LINE_SATURATED);
    assert(!output.download_state_changed);
}

static void test_recovery_requires_confirmation(void)
{
    struct sqm_mon_controller controller;
    struct controller_input high = input_with_rates(
        8000000U,
        8000000U,
        0U,
        8000000U
    );
    struct controller_input low = input_with_rates(
        1000000U,
        8000000U,
        0U,
        8000000U
    );
    struct controller_output output;

    controller_init(&controller);
    update_repeatedly(&controller, &high, &output, 3U);
    update_repeatedly(&controller, &low, &output, 2U);
    assert(output.download_state == CONTROLLER_LINE_SATURATED);
    controller_update(&controller, &low, &output);

    assert(output.download_state == CONTROLLER_LINE_BELOW_CAPACITY);
    assert(output.download_state_changed);
}

static void test_invalid_input_returns_to_unknown(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        1000000U,
        8000000U,
        1000000U,
        8000000U
    );
    struct controller_output output;

    controller_init(&controller);
    controller_update(&controller, &input, &output);
    input.download.valid = false;
    input.upload.cake_rate_bits_per_second = 0U;
    controller_update(&controller, &input, &output);

    assert(output.download_state == CONTROLLER_LINE_UNKNOWN);
    assert(output.upload_state == CONTROLLER_LINE_UNKNOWN);
    assert(output.download_state_changed);
    assert(output.upload_state_changed);
}

static void test_large_rates_do_not_overflow_threshold(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        UINT64_MAX,
        UINT64_MAX,
        0U,
        UINT64_MAX
    );
    struct controller_output output;

    controller_init(&controller);
    update_repeatedly(&controller, &input, &output, 3U);

    assert(output.download_state == CONTROLLER_LINE_SATURATED);
}

static void test_saturated_line_with_delay_detects_congestion(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        8000000U,
        8000000U,
        100000U,
        8000000U
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 45000U;
    controller_init(&controller);
    update_repeatedly(&controller, &input, &output, 3U);

    assert(output.download_state == CONTROLLER_LINE_SATURATED);
    assert(output.download_congestion == CONTROLLER_CONGESTION_DETECTED);
    assert(output.download_congestion_changed);
    assert(output.upload_congestion == CONTROLLER_CONGESTION_CLEAR);
}

static void test_delay_without_saturation_is_clear(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        1000000U,
        8000000U,
        100000U,
        8000000U
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 60000U;
    controller_init(&controller);
    controller_update(&controller, &input, &output);

    assert(output.download_state == CONTROLLER_LINE_BELOW_CAPACITY);
    assert(output.download_congestion == CONTROLLER_CONGESTION_CLEAR);
}

static void test_missing_probe_makes_congestion_unknown(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        8000000U,
        8000000U,
        100000U,
        8000000U
    );
    struct controller_output output;

    controller_init(&controller);
    update_repeatedly(&controller, &input, &output, 3U);
    assert(output.download_congestion == CONTROLLER_CONGESTION_CLEAR);

    input.latency.valid = false;
    controller_update(&controller, &input, &output);

    assert(output.download_state == CONTROLLER_LINE_SATURATED);
    assert(output.download_congestion == CONTROLLER_CONGESTION_UNKNOWN);
    assert(output.download_congestion_changed);
}

static void test_congestion_recovers_with_clean_latency(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        8000000U,
        8000000U,
        100000U,
        8000000U
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 50000U;
    controller_init(&controller);
    update_repeatedly(&controller, &input, &output, 3U);
    assert(output.download_congestion == CONTROLLER_CONGESTION_DETECTED);

    input.latency.current_rtt_microseconds = 35000U;
    controller_update(&controller, &input, &output);

    assert(output.download_state == CONTROLLER_LINE_SATURATED);
    assert(output.download_congestion == CONTROLLER_CONGESTION_CLEAR);
    assert(output.download_congestion_changed);
}

static void test_congestion_hysteresis_prevents_flapping(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        8000000U,
        8000000U,
        100000U,
        8000000U
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 50000U;
    controller_init(&controller);
    update_repeatedly(&controller, &input, &output, 3U);
    assert(output.download_congestion == CONTROLLER_CONGESTION_DETECTED);

    input.latency.current_rtt_microseconds = 42000U;
    controller_update(&controller, &input, &output);

    assert(output.download_congestion == CONTROLLER_CONGESTION_DETECTED);
    assert(!output.download_congestion_changed);
}

static void test_simultaneous_saturation_reports_both_directions(void)
{
    struct sqm_mon_controller controller;
    struct controller_input input = input_with_rates(
        8000000U,
        8000000U,
        8000000U,
        8000000U
    );
    struct controller_output output;

    input.latency.current_rtt_microseconds = 50000U;
    controller_init(&controller);
    update_repeatedly(&controller, &input, &output, 3U);

    assert(output.download_congestion == CONTROLLER_CONGESTION_DETECTED);
    assert(output.upload_congestion == CONTROLLER_CONGESTION_DETECTED);
}

int main(void)
{
    test_initial_state_is_unknown();
    test_low_load_is_below_capacity();
    test_sustained_download_is_saturated();
    test_brief_burst_does_not_saturate();
    test_upload_is_independent();
    test_hysteresis_prevents_flapping();
    test_recovery_requires_confirmation();
    test_invalid_input_returns_to_unknown();
    test_large_rates_do_not_overflow_threshold();
    test_saturated_line_with_delay_detects_congestion();
    test_delay_without_saturation_is_clear();
    test_missing_probe_makes_congestion_unknown();
    test_congestion_recovers_with_clean_latency();
    test_congestion_hysteresis_prevents_flapping();
    test_simultaneous_saturation_reports_both_directions();
    (void)printf("controller tests passed\n");
    return 0;
}
