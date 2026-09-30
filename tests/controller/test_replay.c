#define _POSIX_C_SOURCE 200809L

/*
 * Replays a cake-autorate processing trace through the controller and checks
 * every recorded decision: shaper rates, delay counts and averages, bufferbloat
 * flags, and serialization-compensated thresholds.
 */
#include "controller/controller.h"

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KILOBIT 1000U
#define WIRE_PACKET_BITS 12000U
#define REPORT_LIMIT 10U

struct trace_sample {
    uint64_t processed_microseconds;
    uint64_t achieved_kbps[2];
    int64_t delta_microseconds[2];
    uint64_t delay_threshold[2];
    uint64_t adjust_up_threshold[2];
    uint64_t adjust_down_threshold[2];
    unsigned int sum_delays[2];
    int64_t average_delta[2];
    int bufferbloat[2];
    uint64_t rate_kbps[2];
};

/* cake-autorate ac75f49 defaults.sh with the trace's rate limits. */
static struct controller_config upstream_config(void)
{
    const struct controller_direction_config direction = {
        .adjust = true,
        .minimum_rate_bits_per_second = 10000U * KILOBIT,
        .base_rate_bits_per_second = 20000U * KILOBIT,
        .maximum_rate_bits_per_second = 50000U * KILOBIT,
        .average_delay_maximum_adjust_up_microseconds = 10000U,
        .delay_threshold_microseconds = 30000U,
        .average_delay_maximum_adjust_down_microseconds = 60000U
    };

    return (struct controller_config) {
        .download = direction,
        .upload = direction,
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

static bool parse_sample(const char *line, struct trace_sample *sample)
{
    return sscanf(
        line,
        "D %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNd64 " %" SCNd64
        " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
        " %u %" SCNd64 " %u %" SCNd64 " %d %d %" SCNu64 " %" SCNu64,
        &sample->processed_microseconds,
        &sample->achieved_kbps[0],
        &sample->achieved_kbps[1],
        &sample->delta_microseconds[0],
        &sample->delta_microseconds[1],
        &sample->delay_threshold[0],
        &sample->adjust_up_threshold[0],
        &sample->adjust_down_threshold[0],
        &sample->delay_threshold[1],
        &sample->adjust_up_threshold[1],
        &sample->adjust_down_threshold[1],
        &sample->sum_delays[0],
        &sample->average_delta[0],
        &sample->sum_delays[1],
        &sample->average_delta[1],
        &sample->bufferbloat[0],
        &sample->bufferbloat[1],
        &sample->rate_kbps[0],
        &sample->rate_kbps[1]
    ) == 19;
}

/* Returns the number of decisions that differ from cake-autorate's. */
static unsigned int replay(const char *path)
{
    static const char *const names[] = { "download", "upload" };
    struct controller controller;
    const struct controller_config config = upstream_config();
    FILE *trace = fopen(path, "r");
    char line[512];
    uint64_t sample_id = 0U;
    uint64_t cake_kbps[2] = { 20000U, 20000U };
    unsigned int samples = 0U;
    unsigned int mismatches = 0U;

    assert(trace != NULL);
    assert(controller_init(&controller, &config) == 0);
    /* The monitor compensates before the first sample, once CAKE is discovered. */
    controller_set_serialization_compensation(
        &controller,
        WIRE_PACKET_BITS,
        WIRE_PACKET_BITS,
        cake_kbps[0] * KILOBIT,
        cake_kbps[1] * KILOBIT
    );
    while (fgets(line, sizeof(line), trace) != NULL) {
        struct trace_sample sample;
        struct controller_input input;
        struct controller_output output;
        const struct controller_direction_output *outputs[2];
        const struct controller_direction *directions[2];
        size_t index;

        if (line[0] == 'L') {
            sample_id++;
            continue;
        }
        if (line[0] != 'D') {
            continue;
        }
        assert(parse_sample(line, &sample));
        samples++;
        input = (struct controller_input) {
            .download = {
                .valid = true,
                .traffic_sample_id = sample_id,
                .traffic_rate_bits_per_second = sample.achieved_kbps[0] * KILOBIT,
                .cake_rate_bits_per_second = cake_kbps[0] * KILOBIT
            },
            .upload = {
                .valid = true,
                .traffic_sample_id = sample_id,
                .traffic_rate_bits_per_second = sample.achieved_kbps[1] * KILOBIT,
                .cake_rate_bits_per_second = cake_kbps[1] * KILOBIT
            },
            .download_latency = { .valid = true, .owd_delta_microseconds = sample.delta_microseconds[0] },
            .upload_latency = { .valid = true, .owd_delta_microseconds = sample.delta_microseconds[1] },
            .timestamp_microseconds = sample.processed_microseconds
        };
        controller_update(&controller, &input, &output);
        controller_set_serialization_compensation(
            &controller,
            WIRE_PACKET_BITS,
            WIRE_PACKET_BITS,
            controller.download.shaper_rate_bits_per_second,
            controller.upload.shaper_rate_bits_per_second
        );
        outputs[0] = &output.download;
        outputs[1] = &output.upload;
        directions[0] = &controller.download;
        directions[1] = &controller.upload;
        for (index = 0U; index < 2U; index++) {
            const struct controller_direction_output *decision = outputs[index];
            const struct controller_direction_config *compensated = &directions[index]->config;
            uint64_t rate_kbps = directions[index]->shaper_rate_bits_per_second / KILOBIT;
            bool bufferbloat = decision->congestion == CONTROLLER_CONGESTION_DETECTED;

            if (
                rate_kbps != sample.rate_kbps[index] ||
                decision->delayed_sample_count != sample.sum_delays[index] ||
                decision->average_delay_microseconds != sample.average_delta[index] ||
                bufferbloat != (sample.bufferbloat[index] != 0) ||
                compensated->delay_threshold_microseconds != sample.delay_threshold[index] ||
                compensated->average_delay_maximum_adjust_up_microseconds != sample.adjust_up_threshold[index] ||
                compensated->average_delay_maximum_adjust_down_microseconds != sample.adjust_down_threshold[index]
            ) {
                if (mismatches < REPORT_LIMIT) {
                    (void)printf(
                        "%s sample %u %s: rate %" PRIu64 "/%" PRIu64
                        " sum %u/%u avg %" PRId64 "/%" PRId64 " bb %d/%d"
                        " thr %" PRIu64 "/%" PRIu64 " up %" PRIu64 "/%" PRIu64
                        " down %" PRIu64 "/%" PRIu64 " (cake-adapt/cake-autorate)\n",
                        path,
                        samples,
                        names[index],
                        rate_kbps,
                        sample.rate_kbps[index],
                        decision->delayed_sample_count,
                        sample.sum_delays[index],
                        decision->average_delay_microseconds,
                        sample.average_delta[index],
                        bufferbloat,
                        sample.bufferbloat[index],
                        compensated->delay_threshold_microseconds,
                        sample.delay_threshold[index],
                        compensated->average_delay_maximum_adjust_up_microseconds,
                        sample.adjust_up_threshold[index],
                        compensated->average_delay_maximum_adjust_down_microseconds,
                        sample.adjust_down_threshold[index]
                    );
                }
                mismatches++;
            }
            /* CAKE holds whatever rate cake-autorate set for this sample. */
            cake_kbps[index] = sample.rate_kbps[index];
        }
    }
    assert(fclose(trace) == 0);
    controller_close(&controller);
    (void)printf("%s: replayed %u samples, %u mismatching decisions\n", path, samples, mismatches);
    return mismatches;
}

int main(void)
{
    static const char *const traces[] = {
        "controller/fixtures/cake-autorate-ac75f49-ab.trace",
        "controller/fixtures/cake-autorate-ac75f49-congestion.trace"
    };
    unsigned int mismatches = 0U;
    size_t index;

    for (index = 0U; index < sizeof(traces) / sizeof(traces[0]); index++) {
        mismatches += replay(traces[index]);
    }
    assert(mismatches == 0U);
    (void)puts("cake-autorate replay tests passed");
    return 0;
}
