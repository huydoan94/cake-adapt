#define _POSIX_C_SOURCE 200809L

#include "latency.h"
#include "common/constants.h"

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const struct latency_tracker_config default_tracker_config = {
    .alpha_baseline_increase_per_million = 1000U,
    .alpha_baseline_decrease_per_million = 900000U,
    .alpha_delta_ewma_per_million = 95000U
};

static void init_tracker(struct latency_tracker *tracker)
{
    assert(tracker_init(tracker, &default_tracker_config) == 0);
}

static void test_initial_state_is_closed(void)
{
    struct latency latency;

    latency_init(&latency);

    assert(latency.children[0].output_descriptor == -1);
    assert(latency.children[0].process_identifier == -1);
    assert(latency_child_count(&latency) == 0U);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(!latency_is_open(&latency));
}

static void test_receive_buffered_output(void)
{
    struct latency latency;
    struct latency_sample sample;
    int descriptors[2];
    char error[256] = "";
    const char first[] = "[123.456000] 1.1.1.1 : [1], 64 bytes, ";
    const char rest[] = "2.50 ms\r\n[123.756000] 1.1.1.1 : [2], timed out\n";

    latency_init(&latency);
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    latency.children[0].output_descriptor = descriptors[0];
    /* This fixture owns only a pipe, not an fping child. */
    latency.children[0].process_identifier = getpid();
    latency.child_count = 1U;

    assert(latency_receive_child(
        &latency,
        1U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_ERROR);
    assert(strstr(error, "not running") != NULL);

    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_PENDING);
    assert(write(descriptors[1], first, sizeof(first) - 1U) == (ssize_t)(sizeof(first) - 1U));
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_PENDING);
    assert(latency.children[0].output_length == sizeof(first) - 1U);
    assert(write(descriptors[1], rest, sizeof(rest) - 1U) == (ssize_t)(sizeof(rest) - 1U));
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_SUCCESS);
    assert(sample.sequence == 1U);
    assert(sample.download_owd_microseconds == 1250U);
    assert(sample.upload_owd_microseconds == 1250U);
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_TIMEOUT);
    assert(sample.sequence == 2U);
    assert(latency.children[0].output_length == 0U);

    assert(write(descriptors[1], "bad\n", 4U) == 4);
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_ERROR);
    assert(strstr(error, "unexpected fping output") != NULL);

    memset(latency.children[0].output_buffer, 'x', sizeof(latency.children[0].output_buffer));
    latency.children[0].output_length = sizeof(latency.children[0].output_buffer);
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_ERROR);
    assert(strstr(error, "too long") != NULL);
    latency.children[0].output_length = 0U;

    assert(close(descriptors[1]) == 0);
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_ERROR);
    assert(strstr(error, "output closed") != NULL);
    assert(close(descriptors[0]) == 0);
}

static void test_invalid_target_is_rejected_before_starting_fping(void)
{
    struct latency latency;
    const char *targets[] = { "not an endpoint" };
    char error[256] = "";

    latency_init(&latency);

    assert(latency_open(
        &latency,
        "lo",
        targets,
        1U,
        1000000U,
        "",
        "",
        error,
        sizeof(error)
    ) != 0);
    assert(latency.children[0].output_descriptor == -1);
    assert(latency.children[0].process_identifier == -1);
    assert(strlen(error) > 0U);
}

static void test_empty_target_list_is_rejected(void)
{
    struct latency latency;
    char error[256] = "";

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        NULL,
        0U,
        1000000U,
        "",
        "",
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "at least one target") != NULL);
    assert(!latency_is_open(&latency));
}

static void test_sub_millisecond_response_spacing_is_rejected(void)
{
    struct latency latency;
    const char *targets[] = { "1.1.1.1", "9.9.9.9" };
    char error[256] = "";

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        targets,
        2U,
        1999U,
        "",
        "",
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "at least 1 ms per target") != NULL);
    assert(!latency_is_open(&latency));
}

static void test_close_is_idempotent(void)
{
    struct latency latency;

    latency_init(&latency);
    latency_close(&latency);
    latency_close(&latency);

    assert(latency.children[0].output_descriptor == -1);
    assert(latency.children[0].process_identifier == -1);
}

static void test_close_releases_every_owned_descriptor(void)
{
    struct latency latency;
    int first[2];
    int second[2];

    latency_init(&latency);
    assert(pipe(first) == 0);
    assert(pipe(second) == 0);
    latency.children[0].output_descriptor = first[0];
    latency.children[1].output_descriptor = second[0];
    latency.child_count = 2U;
    latency_close(&latency);
    assert(fcntl(first[0], F_GETFD) == -1);
    assert(errno == EBADF);
    assert(fcntl(second[0], F_GETFD) == -1);
    assert(errno == EBADF);
    assert(latency_child_count(&latency) == 0U);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(latency_child_descriptor(&latency, CONFIG_MAX_REFLECTORS) == -1);
    assert(close(first[1]) == 0);
    assert(close(second[1]) == 0);
}

static void test_fping_reply_is_parsed(void)
{
    struct latency_sample sample;

    assert(parse_fping_line(
        "[1789284242.09616] 1.1.1.1 : [65536], 64 bytes,"
            " 31.9 ms (31.9 avg, 0% loss)",
        &sample
    ) == LATENCY_FPING_LINE_SAMPLE);
    assert(sample.timestamp_microseconds == UINT64_C(1789284242096160));
    assert(strcmp(sample.target, "1.1.1.1") == 0);
    assert(sample.sequence == UINT64_C(65536));
    assert(sample.download_owd_microseconds == 15950U);
    assert(sample.upload_owd_microseconds == 15950U);
    assert(!sample.timestamp_rollover_sensitive);
}

static void test_irtt_reply_is_parsed_directionally(void)
{
    struct latency_sample sample;

    assert(parse_irtt_line(
        "seq=42 rtt=3ms rd=1.234ms sd=567µs ipdv=0s",
        "2001:db8::1",
        UINT64_C(123456789),
        &sample
    ));
    assert(strcmp(sample.target, "2001:db8::1") == 0);
    assert(sample.sequence == 42U);
    assert(sample.download_owd_microseconds == 1234);
    assert(sample.upload_owd_microseconds == 567);
    assert(sample.timestamp_microseconds == UINT64_C(123456789));
    assert(!sample.timestamp_rollover_sensitive);

    assert(parse_irtt_line(
        "seq=9 rd=1500ns sd=2s",
        "1.1.1.1",
        1U,
        &sample
    ));
    assert(sample.download_owd_microseconds == 2);
    assert(sample.upload_owd_microseconds == 2 * (int64_t)SECOND);
    assert(!parse_irtt_line(
        "seq=9 rd=-1ms sd=2ms",
        "1.1.1.1",
        1U,
        &sample
    ));
    assert(!parse_irtt_line(
        "seq=9 rd=1ms",
        "1.1.1.1",
        1U,
        &sample
    ));
}

static void test_irtt_receive_ignores_non_sample_lines(void)
{
    struct latency latency;
    struct latency_sample sample;
    const char output[] = "IRTT client\nseq=7 rtt=3ms rd=1ms sd=2ms\n";
    char error[256] = "";
    int descriptors[2];

    latency_init(&latency);
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    latency.backend = LATENCY_BACKEND_IRTT;
    latency.active = true;
    latency.child_count = 1U;
    latency.children[0].output_descriptor = descriptors[0];
    latency.children[0].process_identifier = getpid();
    latency.children[0].target = "9.9.9.9";
    assert(write(descriptors[1], output, sizeof(output) - 1U) ==
        (ssize_t)(sizeof(output) - 1U));
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_SUCCESS);
    assert(strcmp(sample.target, "9.9.9.9") == 0);
    assert(sample.sequence == 7U);
    assert(sample.download_owd_microseconds == 1000);
    assert(sample.upload_owd_microseconds == 2000);
    assert(sample.timestamp_microseconds > 0U);
    assert(close(descriptors[1]) == 0);
    assert(close(descriptors[0]) == 0);
}

static void test_fping_six_digit_timestamp_is_preserved(void)
{
    struct latency_sample sample;

    assert(parse_fping_line(
        "[1789284242.000123] 9.9.9.9 : [7], 64 bytes,"
            " 0.125 ms (0.125 avg, 0% loss)",
        &sample
    ) == LATENCY_FPING_LINE_SAMPLE);
    assert(sample.timestamp_microseconds == UINT64_C(1789284242000123));
    assert(strcmp(sample.target, "9.9.9.9") == 0);
    assert(sample.sequence == 7U);
    assert(sample.download_owd_microseconds == 62U);
    assert(sample.upload_owd_microseconds == 62U);
}

static void test_fping_odd_and_extreme_rtt_use_equal_owd_halves(void)
{
    struct latency_sample sample;

    assert(parse_fping_line(
        "[1.000001] 1.1.1.1 : [1], 64 bytes, 1.001 ms",
        &sample
    ) == LATENCY_FPING_LINE_SAMPLE);
    assert(sample.download_owd_microseconds == 500U);
    assert(sample.upload_owd_microseconds == 500U);

    assert(parse_fping_line(
        "[1.000001] 1.1.1.1 : [2], 64 bytes, 4294967.296 ms",
        &sample
    ) == LATENCY_FPING_LINE_SAMPLE);
    assert(sample.download_owd_microseconds == UINT32_MAX / 2U);
    assert(sample.upload_owd_microseconds == UINT32_MAX / 2U);
}

static void test_fping_byte_count_syntax(void)
{
    const char *const invalid[] = {
        "[123.000001] 1.1.1.1 : [1],  bytes, 1.0 ms",
        "[123.000001] 1.1.1.1 : [1], -64 bytes, 1.0 ms",
        "[123.000001] 1.1.1.1 : [1], 64x bytes, 1.0 ms",
        "[123.000001] 1.1.1.1 : [1], 64 byte, 1.0 ms"
    };
    struct latency_sample sample;

    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        assert(parse_fping_line(invalid[index], &sample) == LATENCY_FPING_LINE_INVALID);
    }
    assert(parse_fping_line(
        "[123.000001] 1.1.1.1 : [1], 0 bytes, 1.0 ms",
        &sample
    ) == LATENCY_FPING_LINE_SAMPLE);
}

static void test_fping_timestamp_boundaries(void)
{
    const struct {
        const char *text;
        uint64_t microseconds;
    } valid[] = {
        { "0.1", 100000U },
        { "12.00000123", 12000001U },
        { "12.12345678901234567890", 12123456U },
        { "18446744073709.551615", UINT64_MAX }
    };
    const char *const invalid[] = {
        "12.", "12.1x", "12.-1", ".1", "12. 1",
        "18446744073709.551616", "18446744073709551616.0"
    };
    struct latency_sample sample;
    char line[128];

    for (size_t index = 0U; index < sizeof(valid) / sizeof(valid[0]); index++) {
        (void)snprintf(line, sizeof(line), "[%s] 1.1.1.1 : [1], 64 bytes, 1.0 ms", valid[index].text);
        assert(parse_fping_line(line, &sample) == LATENCY_FPING_LINE_SAMPLE);
        assert(sample.timestamp_microseconds == valid[index].microseconds);
    }
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        (void)snprintf(line, sizeof(line), "[%s] 1.1.1.1 : [1], 64 bytes, 1.0 ms", invalid[index]);
        assert(parse_fping_line(line, &sample) == LATENCY_FPING_LINE_INVALID);
    }
}

static void test_fping_timeout_is_recognized(void)
{
    struct latency_sample sample;

    assert(parse_fping_line(
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

    assert(parse_fping_line(
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
        .download_owd_microseconds = round_trip_microseconds / 2U,
        .upload_owd_microseconds = round_trip_microseconds / 2U
    };
    struct latency_observation observation;

    tracker_update(
        tracker,
        &sample,
        &observation
    );
    tracker_update_delta_ewma(tracker, true, &observation);
    return observation;
}

static void test_first_sample_updates_initialized_baseline(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    observation = track(&tracker, 30000U);

    assert(observation.download_owd_microseconds + observation.upload_owd_microseconds == 30000U);
    assert(observation.download_owd_microseconds == 15000U);
    assert(observation.download_owd_baseline_microseconds == 23500U);
    assert(observation.download_owd_delta_microseconds == -8500);
    assert(observation.download_owd_delta_ewma_microseconds == -807);
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

    assert(tracker_init(&tracker, &config) == 0);
    observation = track(&tracker, 300000U);
    assert(observation.download_owd_baseline_microseconds == 110000U);
    assert(observation.download_owd_delta_microseconds == 40000);
    assert(observation.download_owd_delta_ewma_microseconds == 20000);

    observation = track(&tracker, 100000U);
    assert(observation.download_owd_baseline_microseconds == 80000U);
    assert(observation.download_owd_delta_microseconds == -30000);
    assert(observation.download_owd_delta_ewma_microseconds == -5000);
}

static void test_delta_ewma_freezes_during_load(void)
{
    struct latency_tracker tracker;
    struct latency_sample sample = {
        .download_owd_microseconds = 120000U,
        .upload_owd_microseconds = 120000U
    };
    struct latency_observation observation;

    init_tracker(&tracker);
    tracker_update(&tracker, &sample, &observation);
    tracker_update_delta_ewma(&tracker, false, &observation);

    assert(observation.download_owd_delta_microseconds == 19980);
    assert(observation.download_owd_delta_ewma_microseconds == 0);
}

static void test_asymmetric_tracker_state_evolves_independently(void)
{
    struct latency_tracker tracker;
    const struct latency_sample sample = {
        .download_owd_microseconds = 200000U,
        .upload_owd_microseconds = 20000U
    };
    struct latency_observation observation;

    init_tracker(&tracker);
    tracker_update(&tracker, &sample, &observation);
    tracker_update_delta_ewma(&tracker, true, &observation);

    assert(observation.download_owd_baseline_microseconds == 100100U);
    assert(observation.download_owd_delta_microseconds == 99900);
    assert(observation.download_owd_delta_ewma_microseconds == 9490);
    assert(observation.upload_owd_baseline_microseconds == 28000U);
    assert(observation.upload_owd_delta_microseconds == -8000);
    assert(observation.upload_owd_delta_ewma_microseconds == -760);
}

static void test_signed_asymmetric_tracker_handles_one_day_values(void)
{
    struct latency_tracker tracker;
    const struct latency_sample sample = {
        .download_owd_microseconds = -(INT64_C(24) * 60 * 60 * 1000000),
        .upload_owd_microseconds = INT64_C(24) * 60 * 60 * 1000000
    };
    struct latency_observation observation;

    init_tracker(&tracker);
    tracker_update(&tracker, &sample, &observation);
    tracker_update_delta_ewma(&tracker, true, &observation);

    assert(observation.download_owd_baseline_microseconds == -INT64_C(77759990000));
    assert(observation.download_owd_delta_microseconds == -INT64_C(8640010000));
    assert(observation.download_owd_delta_ewma_microseconds == -INT64_C(820800950));
    assert(observation.upload_owd_baseline_microseconds == INT64_C(86499900));
    assert(observation.upload_owd_delta_microseconds == INT64_C(86313500100));
    assert(observation.upload_owd_delta_ewma_microseconds == INT64_C(8199782509));
}

static void test_invalid_alpha_is_rejected(void)
{
    struct latency_tracker_config config = default_tracker_config;
    struct latency_tracker tracker;

    config.alpha_delta_ewma_per_million = 1000001U;
    assert(tracker_init(&tracker, &config) != 0);
}

static void test_lower_sample_reduces_baseline(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 30000U);
    observation = track(&tracker, 25000U);

    assert(observation.download_owd_baseline_microseconds == 13600U);
    assert(observation.download_owd_delta_microseconds == -1100);
    assert(observation.download_owd_delta_ewma_microseconds == -834);
}

static void test_higher_sample_reports_delta(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 200000U);
    observation = track(&tracker, 240000U);

    assert(observation.download_owd_baseline_microseconds == 100020U);
    assert(observation.download_owd_delta_microseconds == 19980U);
    assert(observation.download_owd_delta_ewma_microseconds == 1898U);
}

static void test_baseline_increases_slowly(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 200000U);
    observation = track(&tracker, 220000U);

    assert(observation.download_owd_baseline_microseconds == 100010U);
    assert(observation.download_owd_delta_microseconds == 9990U);
}

static void test_maximum_rtt_does_not_overflow_delta(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 200000U);
    observation = track(&tracker, UINT32_MAX);

    assert(observation.download_owd_microseconds == 2147483647U);
    assert(observation.download_owd_baseline_microseconds == 2247383U);
    assert(observation.download_owd_delta_microseconds == INT64_C(2145236264));
}

static void test_reflector_health_uses_rolling_offence_window(void)
{
    const struct reflector_health_config config = {
        .response_deadline_microseconds = 1000000U,
        .detection_window = 4U,
        .detection_threshold = 2U
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

static void test_reflector_health_rejects_invalid_window(void)
{
    struct reflector_health_config config = {
        .response_deadline_microseconds = 1000000U,
        .detection_window = 2U,
        .detection_threshold = 3U
    };
    struct reflector_health health = { 0 };

    assert(health_init(&health, &config, 0U) != 0);
    config.detection_window = 0U;
    config.detection_threshold = 0U;
    assert(health_init(&health, &config, 0U) != 0);
}

static void test_latency_tracker_reset_discards_measurements(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;

    init_tracker(&tracker);
    (void)track(&tracker, 30000U);
    tracker_reset(&tracker);
    observation = track(&tracker, 200000U);

    assert(observation.download_owd_baseline_microseconds == 100000U);
    assert(observation.download_owd_delta_ewma_microseconds == 0);
}

static void test_reflector_health_reset_clears_offences(void)
{
    const struct reflector_health_config config = {
        .response_deadline_microseconds = 100U,
        .detection_window = 2U,
        .detection_threshold = 1U
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

    for (index = 0U; index < 3U; index++) {
        init_tracker(&trackers[index]);
    }
    trackers[0].download.baseline_microseconds = trackers[0].upload.baseline_microseconds = 90000U;
    trackers[0].download.delta_ewma_microseconds = trackers[0].upload.delta_ewma_microseconds = -100;
    trackers[1].download.baseline_microseconds = trackers[1].upload.baseline_microseconds = 1U;
    trackers[1].download.delta_ewma_microseconds = trackers[1].upload.delta_ewma_microseconds = -1000;
    trackers[2].download.baseline_microseconds = trackers[2].upload.baseline_microseconds = 100000U;
    trackers[2].download.delta_ewma_microseconds = trackers[2].upload.delta_ewma_microseconds = 400;

    reflector_compare(trackers, order, 2U, comparisons);
    assert(comparisons[0].minimum_sum_owd_baselines_microseconds == 180000U);
    assert(comparisons[0].sum_owd_baselines_microseconds == 200000U);
    assert(comparisons[0].sum_owd_baselines_delta_microseconds == 20000U);
    assert(comparisons[0].minimum_download_delta_ewma_microseconds == -100);
    assert(comparisons[0].download_delta_ewma_microseconds == 400);
    assert(comparisons[0].download_delta_ewma_delta_microseconds == 500);
    assert(comparisons[1].sum_owd_baselines_delta_microseconds == 0U);
    assert(comparisons[1].minimum_download_delta_ewma_microseconds == -100);
    assert(comparisons[1].download_delta_ewma_microseconds == -100);
    assert(comparisons[1].download_delta_ewma_delta_microseconds == 0);
}

static void test_reflector_comparison_keeps_directional_minima(void)
{
    struct latency_tracker trackers[3];
    const size_t order[] = { 2U, 0U };
    struct reflector_comparison comparisons[2];
    size_t index;

    for (index = 0U; index < 3U; index++) {
        init_tracker(&trackers[index]);
    }
    trackers[0].download.baseline_microseconds = 50000U;
    trackers[0].upload.baseline_microseconds = 170000U;
    trackers[0].download.delta_ewma_microseconds = 100;
    trackers[0].upload.delta_ewma_microseconds = -500;
    trackers[1].download.baseline_microseconds = 1U;
    trackers[1].upload.baseline_microseconds = 1U;
    trackers[1].download.delta_ewma_microseconds = -1000;
    trackers[1].upload.delta_ewma_microseconds = -1000;
    trackers[2].download.baseline_microseconds = 100000U;
    trackers[2].upload.baseline_microseconds = 80000U;
    trackers[2].download.delta_ewma_microseconds = -200;
    trackers[2].upload.delta_ewma_microseconds = 400;

    reflector_compare(trackers, order, 2U, comparisons);
    assert(comparisons[0].minimum_sum_owd_baselines_microseconds == 180000U);
    assert(comparisons[0].sum_owd_baselines_microseconds == 180000U);
    assert(comparisons[0].minimum_download_delta_ewma_microseconds == -200);
    assert(comparisons[0].download_delta_ewma_delta_microseconds == 0);
    assert(comparisons[0].minimum_upload_delta_ewma_microseconds == -500);
    assert(comparisons[0].upload_delta_ewma_delta_microseconds == 900);
    assert(comparisons[1].sum_owd_baselines_delta_microseconds == 40000U);
    assert(comparisons[1].download_delta_ewma_delta_microseconds == 300);
    assert(comparisons[1].upload_delta_ewma_delta_microseconds == 0);
}

static void test_reflector_comparison_preserves_signed_baselines(void)
{
    struct latency_tracker trackers[2];
    const size_t order[] = { 0U, 1U };
    struct reflector_comparison comparisons[2];

    init_tracker(&trackers[0]);
    init_tracker(&trackers[1]);
    trackers[0].download.baseline_microseconds = -INT64_C(40000000000);
    trackers[0].upload.baseline_microseconds = INT64_C(10000000000);
    trackers[0].download.delta_ewma_microseconds = INT64_C(5000000000);
    trackers[0].upload.delta_ewma_microseconds = -INT64_C(6000000000);
    trackers[1].download.baseline_microseconds = -INT64_C(50000000000);
    trackers[1].upload.baseline_microseconds = -INT64_C(20000000000);
    trackers[1].download.delta_ewma_microseconds = -INT64_C(7000000000);
    trackers[1].upload.delta_ewma_microseconds = INT64_C(8000000000);

    reflector_compare(trackers, order, 2U, comparisons);
    assert(comparisons[0].minimum_sum_owd_baselines_microseconds == -INT64_C(70000000000));
    assert(comparisons[0].sum_owd_baselines_microseconds == -INT64_C(30000000000));
    assert(comparisons[0].sum_owd_baselines_delta_microseconds == UINT64_C(40000000000));
    assert(comparisons[0].download_delta_ewma_delta_microseconds == INT64_C(12000000000));
    assert(comparisons[0].upload_delta_ewma_delta_microseconds == 0);
}

static void test_reflector_comparison_separates_all_minima(void)
{
    struct latency_tracker trackers[3];
    const size_t order[] = { 0U, 1U, 2U };
    struct reflector_comparison comparisons[3];
    size_t index;

    for (index = 0U; index < 3U; index++) {
        init_tracker(&trackers[index]);
    }
    trackers[0].download.baseline_microseconds = 20U;
    trackers[0].upload.baseline_microseconds = 120U;
    trackers[0].download.delta_ewma_microseconds = 50;
    trackers[0].upload.delta_ewma_microseconds = 9;
    trackers[1].download.baseline_microseconds = 90U;
    trackers[1].upload.baseline_microseconds = 70U;
    trackers[1].download.delta_ewma_microseconds = -5;
    trackers[1].upload.delta_ewma_microseconds = 30;
    trackers[2].download.baseline_microseconds = 200U;
    trackers[2].upload.baseline_microseconds = 100U;
    trackers[2].download.delta_ewma_microseconds = 10;
    trackers[2].upload.delta_ewma_microseconds = -10;

    reflector_compare(trackers, order, 3U, comparisons);
    assert(comparisons[0].minimum_sum_owd_baselines_microseconds == 140);
    assert(comparisons[0].minimum_download_delta_ewma_microseconds == -5);
    assert(comparisons[0].minimum_upload_delta_ewma_microseconds == -10);
    assert(comparisons[0].sum_owd_baselines_delta_microseconds == 0U);
    assert(comparisons[1].sum_owd_baselines_delta_microseconds == 20U);
    assert(comparisons[1].download_delta_ewma_delta_microseconds == 0);
    assert(comparisons[2].upload_delta_ewma_delta_microseconds == 0);
}

static void test_reflector_comparison_saturates_signed_extremes(void)
{
    struct latency_tracker trackers[2];
    const size_t order[] = { 0U, 1U };
    struct reflector_comparison comparisons[2];

    init_tracker(&trackers[0]);
    init_tracker(&trackers[1]);
    trackers[0].download.baseline_microseconds = INT64_MAX;
    trackers[0].upload.baseline_microseconds = INT64_MAX;
    trackers[0].download.delta_ewma_microseconds = INT64_MAX;
    trackers[0].upload.delta_ewma_microseconds = INT64_MAX;
    trackers[1].download.baseline_microseconds = INT64_MIN;
    trackers[1].upload.baseline_microseconds = INT64_MIN;
    trackers[1].download.delta_ewma_microseconds = INT64_MIN;
    trackers[1].upload.delta_ewma_microseconds = INT64_MIN;

    reflector_compare(trackers, order, 2U, comparisons);
    assert(comparisons[0].sum_owd_baselines_microseconds == INT64_MAX);
    assert(comparisons[0].sum_owd_baselines_delta_microseconds == UINT64_MAX);
    assert(comparisons[0].download_delta_ewma_delta_microseconds == INT64_MAX);
    assert(comparisons[0].upload_delta_ewma_delta_microseconds == INT64_MAX);
    assert(comparisons[1].sum_owd_baselines_microseconds == INT64_MIN);
}

static void test_reflector_rotation_retains_or_discards_identity_state(void)
{
    struct latency_tracker trackers[3];
    size_t order[] = { 0U, 1U, 2U };
    size_t index;

    for (index = 0U; index < 3U; index++) {
        init_tracker(&trackers[index]);
    }
    trackers[0].download.baseline_microseconds = 123;
    trackers[0].upload.baseline_microseconds = 456;
    trackers[0].download.delta_ewma_microseconds = 7;
    trackers[0].upload.delta_ewma_microseconds = 8;

    /* Retention leaves state on the immutable reflector identity. */
    reflector_rotate(order, 3U, 1U, 0U);
    reflector_rotate(order, 3U, 1U, 0U);
    reflector_rotate(order, 3U, 1U, 0U);
    assert(order[0] == 0U);
    assert(trackers[0].download.baseline_microseconds == 123);
    assert(trackers[0].upload.baseline_microseconds == 456);
    assert(trackers[0].download.delta_ewma_microseconds == 7);
    assert(trackers[0].upload.delta_ewma_microseconds == 8);

    /* Discarding resets the departing identity before its next active turn. */
    tracker_reset(&trackers[0]);
    reflector_rotate(order, 3U, 1U, 0U);
    reflector_rotate(order, 3U, 1U, 0U);
    reflector_rotate(order, 3U, 1U, 0U);
    assert(order[0] == 0U);
    assert(trackers[0].download.baseline_microseconds == INT64_C(100000));
    assert(trackers[0].upload.baseline_microseconds == INT64_C(100000));
    assert(trackers[0].download.delta_ewma_microseconds == 0);
    assert(trackers[0].upload.delta_ewma_microseconds == 0);
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

static void test_pinger_arguments_reject_command_substitution(void)
{
    struct latency latency;
    const char *targets[] = { "1.1.1.1" };
    char error[256] = "";

    latency_init(&latency);
    assert(latency_open(&latency, "lo", targets, 1U, 1000000U, "$(id)", "", error, sizeof(error)) != 0);
    assert(strstr(error, "ping_extra_args") != NULL);
    assert(!latency_is_open(&latency));
    assert(latency_open(
        &latency,
        "lo",
        targets,
        1U,
        1000000U,
        "",
        "'unterminated",
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "ping_prefix_string") != NULL);
    assert(!latency_is_open(&latency));
}

static size_t open_descriptor_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    size_t count = 0U;

    assert(directory != NULL);
    while (readdir(directory) != NULL) {
        count++;
    }
    assert(closedir(directory) == 0);
    return count;
}

static void test_failed_spawn_closes_pipe(void)
{
    struct latency latency;
    const char *targets[] = { "127.0.0.1" };
    char error[256];
    size_t descriptors = open_descriptor_count();

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        targets,
        1U,
        1000000U,
        "",
        "/nonexistent-cake-adapt-test/fping",
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "could not start fping") != NULL);
    assert(!latency_is_open(&latency));
    assert(open_descriptor_count() == descriptors);
}

static void test_prefix_and_extra_args_reach_owned_process(void)
{
    struct latency latency;
    const char *targets[] = { "1.1.1.1", "::1" };
    char error[256] = "";
    char output[1024];
    size_t length = 0U;
    struct pollfd descriptor;

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        targets,
        2U,
        300000U,
        "-I 'lo2' -k 768",
        "/usr/bin/printf '%s\\n'",
        error,
        sizeof(error)
    ) == 0);
    descriptor = (struct pollfd) { .fd = latency.children[0].output_descriptor, .events = POLLIN };
    for (;;) {
        ssize_t bytes;

        assert(poll(&descriptor, 1U, 1000) > 0);
        assert(length < sizeof(output) - 1U);
        bytes = read(latency.children[0].output_descriptor, output + length, sizeof(output) - 1U - length);
        assert(bytes >= 0);
        if (bytes == 0) {
            break;
        }
        length += (size_t)bytes;
    }
    output[length] = '\0';
    assert(strcmp(
        output,
        "/usr/bin/fping\n-I\nlo2\n-k\n768\n--timestamp\n--loop\n"
        "--period\n300\n--interval\n150\n--timeout\n10000\n1.1.1.1\n::1\n"
    ) == 0);
    latency_close(&latency);
    assert(!latency_is_open(&latency));
    assert(latency_child_count(&latency) == 0U);
    assert(target_is_valid("::1"));
    assert(target_is_valid("2001:4860:4860::8888"));
}

static void test_irtt_children_start_in_separate_slots(void)
{
    struct latency latency;
    struct latency_sample sample;
    const char *targets[] = { "1.1.1.1", "2001:db8::1" };
    char error[256] = "";
    char output[1024];
    size_t length = 0U;
    struct pollfd descriptor;

    latency_init(&latency);
    assert(latency_open_irtt(
        &latency,
        targets,
        2U,
        300U * MILLISECOND,
        10U,
        "--fill=rand",
        "/usr/bin/printf '%s\\n'",
        1000U,
        error,
        sizeof(error)
    ) == 0);
    assert(latency_is_open(&latency));
    assert(latency_child_count(&latency) == 2U);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(latency_child_descriptor(&latency, 1U) == -1);
    assert(latency_irtt_start_pending(&latency));
    assert(latency_start_irtt_children(
        &latency,
        999U,
        error,
        sizeof(error)
    ) == 0);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(latency_child_descriptor(&latency, 1U) == -1);
    assert(latency_start_irtt_children(
        &latency,
        1000U,
        error,
        sizeof(error)
    ) == 0);
    assert(latency_child_descriptor(&latency, 0U) >= 0);
    assert(latency_child_descriptor(&latency, 1U) == -1);
    assert(latency_irtt_next_start_microseconds(&latency) == 151000U);

    descriptor = (struct pollfd) {
        .fd = latency_child_descriptor(&latency, 0U),
        .events = POLLIN
    };
    for (;;) {
        ssize_t bytes;

        assert(poll(&descriptor, 1U, 1000) > 0);
        bytes = read(
            descriptor.fd,
            output + length,
            sizeof(output) - 1U - length
        );
        assert(bytes >= 0);
        if (bytes == 0) {
            break;
        }
        length += (size_t)bytes;
    }
    output[length] = '\0';
    assert(strcmp(
        output,
        "/usr/bin/irtt\nclient\n--fill=rand\n-i\n0.300000s\n"
            "-d\n10m\n1.1.1.1\n"
    ) == 0);

    assert(latency_start_irtt_children(
        &latency,
        151000U,
        error,
        sizeof(error)
    ) == 0);
    assert(latency_child_descriptor(&latency, 1U) >= 0);
    assert(!latency_irtt_start_pending(&latency));
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_RESTART);
    assert(strstr(error, "irtt exited with status 0") != NULL);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(latency_child_descriptor(&latency, 1U) >= 0);
    assert(latency_irtt_start_pending(&latency));
    latency_close(&latency);
    assert(!latency_is_open(&latency));
}

static void test_timestamp_rollover_resets_only_timestamp_samples(void)
{
    struct latency_tracker tracker;
    struct latency_observation observation;
    struct latency_sample sample = {
        .download_owd_microseconds =
            (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS,
        .upload_owd_microseconds = 0,
        .timestamp_rollover_sensitive = true
    };

    init_tracker(&tracker);
    tracker.download.baseline_microseconds = 0;
    tracker.upload.baseline_microseconds = 0;
    tracker.download.delta_ewma_microseconds = 100;
    tracker.upload.delta_ewma_microseconds = 200;
    tracker_update(&tracker, &sample, &observation);
    assert(observation.download_owd_delta_microseconds == 0);
    assert(observation.upload_owd_delta_microseconds == 0);
    assert(tracker.download.baseline_microseconds == sample.download_owd_microseconds);
    assert(tracker.upload.baseline_microseconds == sample.upload_owd_microseconds);
    tracker_update_delta_ewma(&tracker, true, &observation);
    assert(tracker.download.delta_ewma_microseconds == 90);
    assert(tracker.upload.delta_ewma_microseconds == 181);

    tracker.download.baseline_microseconds = INT64_MAX;
    tracker.upload.baseline_microseconds = INT64_MIN;
    sample.download_owd_microseconds = INT64_MIN;
    sample.upload_owd_microseconds = INT64_MAX;
    tracker_update(&tracker, &sample, &observation);
    assert(observation.download_owd_delta_microseconds == 0);
    assert(observation.upload_owd_delta_microseconds == 0);
    assert(tracker.download.baseline_microseconds == INT64_MIN);
    assert(tracker.upload.baseline_microseconds == INT64_MAX);
    tracker_update(&tracker, &sample, &observation);
    assert(tracker.download.baseline_microseconds == INT64_MIN);
    assert(tracker.upload.baseline_microseconds == INT64_MAX);
    assert(observation.download_owd_delta_microseconds == 0);
    assert(observation.upload_owd_delta_microseconds == 0);
    tracker.download.delta_ewma_microseconds = 500;
    tracker.upload.delta_ewma_microseconds = 600;
    tracker_update_delta_ewma(&tracker, false, &observation);
    assert(tracker.download.delta_ewma_microseconds == 500);
    assert(tracker.upload.delta_ewma_microseconds == 600);

    tracker_reset(&tracker);
    tracker.download.baseline_microseconds = 0;
    tracker.upload.baseline_microseconds = 0;
    sample.download_owd_microseconds =
        (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS - 1;
    sample.upload_owd_microseconds = 0;
    tracker_update(&tracker, &sample, &observation);
    assert(observation.download_owd_delta_microseconds != 0);
    tracker_reset(&tracker);
    tracker.download.baseline_microseconds = 0;
    tracker.upload.baseline_microseconds = 0;
    sample.download_owd_microseconds =
        (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS - 1;
    sample.upload_owd_microseconds = 2;
    tracker_update(&tracker, &sample, &observation);
    assert(observation.download_owd_delta_microseconds == 0);
    assert(observation.upload_owd_delta_microseconds == 0);
    assert(tracker.download.baseline_microseconds == sample.download_owd_microseconds);
    assert(tracker.upload.baseline_microseconds == sample.upload_owd_microseconds);

    tracker.download.baseline_microseconds = 100;
    tracker.upload.baseline_microseconds = -100;
    sample.download_owd_microseconds = 100 +
        (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS / 2;
    sample.upload_owd_microseconds = -100 -
        (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS / 2;
    tracker_update(&tracker, &sample, &observation);
    assert(observation.download_owd_delta_microseconds == 0);
    assert(observation.upload_owd_delta_microseconds == 0);

    tracker.download.baseline_microseconds = 0;
    tracker.upload.baseline_microseconds = 0;
    sample.download_owd_microseconds = 0;
    sample.upload_owd_microseconds =
        (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS;
    tracker_update(&tracker, &sample, &observation);
    assert(observation.download_owd_delta_microseconds == 0);
    assert(observation.upload_owd_delta_microseconds == 0);
    assert(tracker.download.baseline_microseconds == 0);
    assert(tracker.upload.baseline_microseconds == sample.upload_owd_microseconds);

    sample.timestamp_rollover_sensitive = false;
    sample.download_owd_microseconds =
        (int64_t)LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS;
    tracker_reset(&tracker);
    tracker_update(&tracker, &sample, &observation);
    assert(observation.download_owd_delta_microseconds != 0);
}

int main(void)
{
    test_initial_state_is_closed();
    test_receive_buffered_output();
    test_invalid_target_is_rejected_before_starting_fping();
    test_empty_target_list_is_rejected();
    test_sub_millisecond_response_spacing_is_rejected();
    test_close_is_idempotent();
    test_close_releases_every_owned_descriptor();
    test_fping_reply_is_parsed();
    test_irtt_reply_is_parsed_directionally();
    test_irtt_receive_ignores_non_sample_lines();
    test_reflector_comparison_preserves_signed_baselines();
    test_fping_six_digit_timestamp_is_preserved();
    test_fping_byte_count_syntax();
    test_fping_odd_and_extreme_rtt_use_equal_owd_halves();
    test_fping_timestamp_boundaries();
    test_fping_timeout_is_recognized();
    test_fping_reply_identifies_each_target();
    test_signed_asymmetric_tracker_handles_one_day_values();
    test_first_sample_updates_initialized_baseline();
    test_configured_alpha_values_are_used();
    test_delta_ewma_freezes_during_load();
    test_invalid_alpha_is_rejected();
    test_asymmetric_tracker_state_evolves_independently();
    test_lower_sample_reduces_baseline();
    test_higher_sample_reports_delta();
    test_baseline_increases_slowly();
    test_maximum_rtt_does_not_overflow_delta();
    test_reflector_health_uses_rolling_offence_window();
    test_reflector_health_rejects_invalid_window();
    test_latency_tracker_reset_discards_measurements();
    test_reflector_health_reset_clears_offences();
    test_reflector_comparison_uses_active_order();
    test_reflector_comparison_separates_all_minima();
    test_reflector_comparison_saturates_signed_extremes();
    test_reflector_rotation_retains_or_discards_identity_state();
    test_reflector_rotation_uses_first_standby();
    test_reflector_comparison_keeps_directional_minima();
    test_pinger_arguments_reject_command_substitution();
    test_failed_spawn_closes_pipe();
    test_prefix_and_extra_args_reach_owned_process();
    test_irtt_children_start_in_separate_slots();
    test_timestamp_rollover_resets_only_timestamp_samples();

    (void)puts("latency tests passed");
    return 0;
}
