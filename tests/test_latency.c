#include "latency.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct latency_tracker_config default_tracker_config = {
    .alpha_baseline_increase_per_million = 1000U,
    .alpha_baseline_decrease_per_million = 900000U,
    .alpha_delta_ewma_per_million = 95000U
};

static void init_tracker(struct latency_tracker *tracker)
{
    assert(latency_tracker_init(tracker, &default_tracker_config) == 0);
}

static void test_initial_state_is_closed(void)
{
    struct sqm_mon_latency latency;

    latency_init(&latency);

    assert(latency.output_descriptor == -1);
    assert(latency.process_identifier == -1);
    assert(!latency_is_open(&latency));
}

static void test_invalid_target_is_rejected_before_starting_fping(void)
{
    struct sqm_mon_latency latency;
    const char *targets[] = { "not an endpoint" };
    char error[256] = "";

    latency_init(&latency);

    assert(latency_open(
        &latency,
        "lo",
        targets,
        1U,
        1000000U,
        error,
        sizeof(error)
    ) != 0);
    assert(latency.output_descriptor == -1);
    assert(latency.process_identifier == -1);
    assert(strlen(error) > 0U);
}

static void test_empty_target_list_is_rejected(void)
{
    struct sqm_mon_latency latency;
    char error[256] = "";

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        NULL,
        0U,
        1000000U,
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "at least one target") != NULL);
    assert(!latency_is_open(&latency));
}

static void test_sub_millisecond_response_spacing_is_rejected(void)
{
    struct sqm_mon_latency latency;
    const char *targets[] = { "1.1.1.1", "9.9.9.9" };
    char error[256] = "";

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        targets,
        2U,
        1999U,
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "at least 1 ms per target") != NULL);
    assert(!latency_is_open(&latency));
}

static void test_close_is_idempotent(void)
{
    struct sqm_mon_latency latency;

    latency_init(&latency);
    latency_close(&latency);
    latency_close(&latency);

    assert(latency.output_descriptor == -1);
    assert(latency.process_identifier == -1);
}

static void test_fping_reply_is_parsed(void)
{
    struct latency_sample sample;

    assert(latency_parse_fping_line(
        "[1789284242.09616] 1.1.1.1 : [65536], 64 bytes,"
            " 31.9 ms (31.9 avg, 0% loss)",
        &sample
    ) == LATENCY_FPING_LINE_SAMPLE);
    assert(sample.timestamp_microseconds == UINT64_C(1789284242096160));
    assert(strcmp(sample.target, "1.1.1.1") == 0);
    assert(sample.sequence == UINT64_C(65536));
    assert(sample.round_trip_microseconds == 31900U);
}

static void test_fping_six_digit_timestamp_is_preserved(void)
{
    struct latency_sample sample;

    assert(latency_parse_fping_line(
        "[1789284242.000123] 9.9.9.9 : [7], 64 bytes,"
            " 0.125 ms (0.125 avg, 0% loss)",
        &sample
    ) == LATENCY_FPING_LINE_SAMPLE);
    assert(sample.timestamp_microseconds == UINT64_C(1789284242000123));
    assert(strcmp(sample.target, "9.9.9.9") == 0);
    assert(sample.sequence == 7U);
    assert(sample.round_trip_microseconds == 125U);
}

static void test_fping_timeout_is_recognized(void)
{
    struct latency_sample sample;

    assert(latency_parse_fping_line(
        "[1789284242.09616] 1.1.1.1 : [8], timed out"
            " (NaN avg, 100% loss)",
        &sample
    ) == LATENCY_FPING_LINE_TIMEOUT);
    assert(strcmp(sample.target, "1.1.1.1") == 0);
    assert(sample.sequence == 8U);
}

static void test_fping_reply_identifies_each_target(void)
{
    struct latency_sample sample;

    assert(latency_parse_fping_line(
        "[1789284242.09616] 9.9.9.9  : [8], 64 bytes,"
            " 31.9 ms (31.9 avg, 0% loss)",
        &sample
    ) == LATENCY_FPING_LINE_SAMPLE);
    assert(strcmp(sample.target, "9.9.9.9") == 0);
}

static struct latency_observation track(
    struct latency_tracker *tracker,
    uint32_t round_trip_microseconds
)
{
    struct latency_sample sample = {
        .round_trip_microseconds = round_trip_microseconds
    };
    struct latency_observation observation;

    latency_tracker_update(
        tracker,
        &sample,
        &observation
    );
    latency_tracker_update_delta_ewma(tracker, true, &observation);
    return observation;
}

static void test_first_sample_updates_initialized_baseline(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    observation = track(&tracker, 30000U);

    assert(observation.round_trip_microseconds == 30000U);
    assert(observation.one_way_microseconds == 15000U);
    assert(observation.one_way_baseline_microseconds == 23500U);
    assert(observation.one_way_delta_microseconds == -8500);
    assert(observation.one_way_delta_ewma_microseconds == -807);
}

static void test_configured_alpha_values_are_used(void)
{
    const struct latency_tracker_config config = {
        .alpha_baseline_increase_per_million = 200000U,
        .alpha_baseline_decrease_per_million = 500000U,
        .alpha_delta_ewma_per_million = 500000U
    };
    struct latency_tracker tracker;
    struct latency_observation observation;

    assert(latency_tracker_init(&tracker, &config) == 0);
    observation = track(&tracker, 300000U);
    assert(observation.one_way_baseline_microseconds == 110000U);
    assert(observation.one_way_delta_microseconds == 40000);
    assert(observation.one_way_delta_ewma_microseconds == 20000);

    observation = track(&tracker, 100000U);
    assert(observation.one_way_baseline_microseconds == 80000U);
    assert(observation.one_way_delta_microseconds == -30000);
    assert(observation.one_way_delta_ewma_microseconds == -5000);
}

static void test_delta_ewma_freezes_during_load(void)
{
    struct latency_tracker tracker;
    struct latency_sample sample = {
        .round_trip_microseconds = 240000U
    };
    struct latency_observation observation;

    init_tracker(&tracker);
    latency_tracker_update(&tracker, &sample, &observation);
    latency_tracker_update_delta_ewma(&tracker, false, &observation);

    assert(observation.one_way_delta_microseconds == 19980);
    assert(observation.one_way_delta_ewma_microseconds == 0);
}

static void test_invalid_alpha_is_rejected(void)
{
    struct latency_tracker_config config = default_tracker_config;
    struct latency_tracker tracker;

    config.alpha_delta_ewma_per_million = 1000001U;
    assert(latency_tracker_init(&tracker, &config) != 0);
}

static void test_lower_sample_reduces_baseline(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 30000U);
    observation = track(&tracker, 25000U);

    assert(observation.one_way_baseline_microseconds == 13600U);
    assert(observation.one_way_delta_microseconds == -1100);
    assert(observation.one_way_delta_ewma_microseconds == -834);
}

static void test_higher_sample_reports_delta(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 200000U);
    observation = track(&tracker, 240000U);

    assert(observation.one_way_baseline_microseconds == 100020U);
    assert(observation.one_way_delta_microseconds == 19980U);
    assert(observation.one_way_delta_ewma_microseconds == 1898U);
}

static void test_baseline_increases_slowly(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 200000U);
    observation = track(&tracker, 220000U);

    assert(observation.one_way_baseline_microseconds == 100010U);
    assert(observation.one_way_delta_microseconds == 9990U);
}

static void test_maximum_rtt_does_not_overflow_delta(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 200000U);
    observation = track(&tracker, UINT32_MAX);

    assert(observation.one_way_microseconds == 2147483647U);
    assert(observation.one_way_baseline_microseconds == 2247383U);
    assert(observation.one_way_delta_microseconds == INT64_C(2145236264));
}

static void test_reflector_health_uses_rolling_offence_window(void)
{
    const struct reflector_health_config config = {
        .response_deadline_microseconds = 1000000U,
        .detection_window = 4U,
        .detection_threshold = 2U
    };
    struct reflector_health health = { 0 };

    assert(reflector_health_init(&health, &config, 1000000U) == 0);
    assert(reflector_health_check(&health, 2000000U) == REFLECTOR_HEALTHY);
    assert(reflector_health_check(&health, 2000001U) == REFLECTOR_OFFENCE);
    reflector_health_record_response(&health, 2500000U);
    assert(reflector_health_check(&health, 3000000U) == REFLECTOR_HEALTHY);
    assert(reflector_health_check(&health, 3500001U) == REFLECTOR_MISBEHAVING);
    reflector_health_record_response(&health, 4000000U);
    assert(reflector_health_check(&health, 4000000U) == REFLECTOR_MISBEHAVING);
    assert(reflector_health_check(&health, 4000000U) == REFLECTOR_HEALTHY);
    reflector_health_cleanup(&health);
}

static void test_reflector_health_rejects_invalid_window(void)
{
    struct reflector_health_config config = {
        .response_deadline_microseconds = 1000000U,
        .detection_window = 2U,
        .detection_threshold = 3U
    };
    struct reflector_health health = { 0 };

    assert(reflector_health_init(&health, &config, 0U) != 0);
    config.detection_window = 0U;
    config.detection_threshold = 0U;
    assert(reflector_health_init(&health, &config, 0U) != 0);
}

static void test_latency_tracker_reset_discards_measurements(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 30000U);
    latency_tracker_reset(&tracker);
    observation = track(&tracker, 200000U);

    assert(observation.one_way_baseline_microseconds == 100000U);
    assert(observation.one_way_delta_ewma_microseconds == 0);
}

static void test_reflector_health_reset_clears_offences(void)
{
    const struct reflector_health_config config = {
        .response_deadline_microseconds = 100U,
        .detection_window = 2U,
        .detection_threshold = 1U
    };
    struct reflector_health health = { 0 };

    assert(reflector_health_init(&health, &config, 100U) == 0);
    assert(reflector_health_check(&health, 201U) == REFLECTOR_MISBEHAVING);
    reflector_health_reset(&health, 300U);
    assert(reflector_health_check(&health, 400U) == REFLECTOR_HEALTHY);
    assert(health.offence_count == 0U);
    reflector_health_cleanup(&health);
}

int main(void)
{
    test_initial_state_is_closed();
    test_invalid_target_is_rejected_before_starting_fping();
    test_empty_target_list_is_rejected();
    test_sub_millisecond_response_spacing_is_rejected();
    test_close_is_idempotent();
    test_fping_reply_is_parsed();
    test_fping_six_digit_timestamp_is_preserved();
    test_fping_timeout_is_recognized();
    test_fping_reply_identifies_each_target();
    test_first_sample_updates_initialized_baseline();
    test_configured_alpha_values_are_used();
    test_delta_ewma_freezes_during_load();
    test_invalid_alpha_is_rejected();
    test_lower_sample_reduces_baseline();
    test_higher_sample_reports_delta();
    test_baseline_increases_slowly();
    test_maximum_rtt_does_not_overflow_delta();
    test_reflector_health_uses_rolling_offence_window();
    test_reflector_health_rejects_invalid_window();
    test_latency_tracker_reset_discards_measurements();
    test_reflector_health_reset_clears_offences();

    (void)puts("latency tests passed");
    return 0;
}
