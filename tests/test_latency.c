#include "latency.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static void test_first_sample_establishes_baseline(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    latency_tracker_init(&tracker);
    observation = track(&tracker, 30000U);

    assert(observation.round_trip_microseconds == 30000U);
    assert(observation.baseline_microseconds == 30000U);
    assert(observation.delta_microseconds == 0U);
}

static void test_lower_sample_reduces_baseline(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    latency_tracker_init(&tracker);
    (void)track(&tracker, 30000U);
    observation = track(&tracker, 25000U);

    assert(observation.baseline_microseconds == 25500U);
    assert(observation.delta_microseconds == -500);
    assert(observation.delta_ewma_microseconds == -23);
}

static void test_higher_sample_reports_delta(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    latency_tracker_init(&tracker);
    (void)track(&tracker, 25000U);
    observation = track(&tracker, 40000U);

    assert(observation.baseline_microseconds == 25015U);
    assert(observation.delta_microseconds == 14985U);
    assert(observation.delta_ewma_microseconds == 711U);
}

static void test_baseline_increases_slowly(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    latency_tracker_init(&tracker);
    (void)track(&tracker, 10000U);
    observation = track(&tracker, 20000U);

    assert(observation.baseline_microseconds == 10010U);
    assert(observation.delta_microseconds == 9990U);
}

static void test_maximum_rtt_does_not_overflow_delta(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    latency_tracker_init(&tracker);
    (void)track(&tracker, 1U);
    observation = track(&tracker, UINT32_MAX);

    assert(observation.baseline_microseconds == 4294968U);
    assert(observation.delta_microseconds == INT64_C(4290672327));
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
    test_first_sample_establishes_baseline();
    test_lower_sample_reduces_baseline();
    test_higher_sample_reports_delta();
    test_baseline_increases_slowly();
    test_maximum_rtt_does_not_overflow_delta();

    (void)puts("latency tests passed");
    return 0;
}
