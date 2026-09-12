#include "latency.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_initial_state_is_closed(void)
{
    struct sqm_mon_latency latency;

    latency_init(&latency);

    assert(latency.socket_descriptor == -1);
}

static void test_invalid_target_is_rejected_before_opening_socket(void)
{
    struct sqm_mon_latency latency;
    char error[256] = "";

    latency_init(&latency);

    assert(latency_open(
        &latency,
        "lo",
        "not-an-ip-address",
        error,
        sizeof(error)
    ) != 0);
    assert(latency.socket_descriptor == -1);
    assert(strlen(error) > 0U);
}

static void test_close_is_idempotent(void)
{
    struct sqm_mon_latency latency;

    latency_init(&latency);
    latency_close(&latency);
    latency_close(&latency);

    assert(latency.socket_descriptor == -1);
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

    assert(observation.baseline_microseconds == 25000U);
    assert(observation.delta_microseconds == 0U);
}

static void test_higher_sample_reports_delta(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    latency_tracker_init(&tracker);
    (void)track(&tracker, 25000U);
    observation = track(&tracker, 40000U);

    assert(observation.baseline_microseconds == 25000U);
    assert(observation.delta_microseconds == 15000U);
}

static void test_baseline_adapts_after_old_minimum_expires(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation = { 0U, 0U, 0U };
    unsigned int index;

    latency_tracker_init(&tracker);
    (void)track(&tracker, 10000U);
    for (index = 0U; index < LATENCY_BASELINE_WINDOW_SAMPLES; index++) {
        observation = track(&tracker, 20000U);
    }

    assert(observation.baseline_microseconds == 20000U);
    assert(observation.delta_microseconds == 0U);
}

static void test_maximum_rtt_does_not_overflow_delta(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    latency_tracker_init(&tracker);
    (void)track(&tracker, 1U);
    observation = track(&tracker, UINT32_MAX);

    assert(observation.baseline_microseconds == 1U);
    assert(observation.delta_microseconds == UINT32_MAX - 1U);
}

int main(void)
{
    test_initial_state_is_closed();
    test_invalid_target_is_rejected_before_opening_socket();
    test_close_is_idempotent();
    test_first_sample_establishes_baseline();
    test_lower_sample_reduces_baseline();
    test_higher_sample_reports_delta();
    test_baseline_adapts_after_old_minimum_expires();
    test_maximum_rtt_does_not_overflow_delta();

    (void)puts("latency tests passed");
    return 0;
}
