#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/*
 * Closed-loop bufferbloat simulator for the cake-adapt controller.
 *
 * Each direction has an ISP bottleneck: a FIFO with a deep buffer draining at
 * a time-varying capacity. CAKE limits the flow to the controller's shaper
 * rate. Greedy TCP fills the shaper while the bottleneck buffer has room; once
 * the buffer overflows, losses hold TCP to the capacity, leaving a standing
 * queue (bufferbloat). Reflector replies carry the bottleneck queueing delay
 * and go through the real tracker and controller, as the daemon feeds them.
 *
 * Upload CAKE shapes before the bottleneck, so its achieved rate is what enters
 * the bottleneck. Download CAKE shapes on the IFB after it, so its achieved
 * rate is what the bottleneck delivers, never more than the capacity.
 *
 * Usage: simulate [--owd] [--trace SCENARIO]
 *   --owd    one-way delays per direction (fping-ts/irtt) instead of RTT/2
 *   --trace  print a per-100 ms CSV for one scenario instead of the summary
 */
#include "controller/controller.h"
#include "latency/tracker.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KILOBIT UINT64_C(1000)
#define MEGABIT UINT64_C(1000000)
#define STEP_US UINT64_C(1000)
#define REFLECTORS 6U
#define REPLY_SPACING_US UINT64_C(50000)
#define TRAFFIC_INTERVAL_US UINT64_C(200000)
#define WIRE_PACKET_BITS UINT64_C(12000)
#define IDLE_RATE_BPS (50.0 * 1000.0)
#define BLOAT_SECONDS 0.5
#define METRIC_SPACING_US UINT64_C(10000)

enum { DL, UL };

struct direction_profile {
    double (*capacity_mbps)(double seconds, unsigned int seed);
    double load_start_seconds;
    double load_end_seconds;
    uint64_t minimum_mbps_x1000;
    uint64_t base_mbps_x1000;
    uint64_t maximum_mbps_x1000;
};

struct scenario {
    const char *name;
    double duration_seconds;
    unsigned int seeds;
    struct direction_profile direction[2];
};

struct bottleneck {
    double queue_bytes;
    double buffer_bytes;
    double cake_bytes;
    double delivered_bits;
    double capacity_bits;
};

struct metrics {
    double *delays_ms;
    size_t count;
    size_t capacity;
    double delivered_bits;
    double capacity_bits;
};

/* xorshift32: deterministic jitter and random-walk capacity. */
static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static double unit_random(uint32_t *state)
{
    return (double)next_random(state) / 4294967296.0;
}

static double steady_download(double t, unsigned int seed) { (void)t; (void)seed; return 70.0; }
static double steady_upload(double t, unsigned int seed) { (void)t; (void)seed; return 15.0; }

static double step_download(double t, unsigned int seed)
{
    (void)seed;
    return t < 60.0 ? 80.0 : (t < 120.0 ? 35.0 : 80.0);
}

static double sine_download(double t, unsigned int seed) { (void)seed; return 60.0 + 25.0 * sin(2.0 * M_PI * t / 60.0); }
static double sine_upload(double t, unsigned int seed) { (void)seed; return 12.0 + 4.0 * sin(2.0 * M_PI * t / 45.0); }

#define WALK_STEPS 4000U

struct walk_cache {
    unsigned int seed;
    bool valid;
    double values[WALK_STEPS];
};

/* LTE-like capacity: a bounded random walk updated every 100 ms. */
static double walk(struct walk_cache *cache, double t, unsigned int seed, double low, double high)
{
    size_t index = (size_t)(t * 10.0);

    if (!cache->valid || cache->seed != seed) {
        uint32_t state = 0x9e3779b9U ^ (seed * 2654435761U) ^ (uint32_t)low;
        double value = (low + high) / 2.0;
        size_t step;

        for (step = 0U; step < WALK_STEPS; step++) {
            value += (unit_random(&state) - 0.5) * (high - low) * 0.06;
            value = value < low ? low : (value > high ? high : value);
            cache->values[step] = value;
        }
        cache->seed = seed;
        cache->valid = true;
    }
    return cache->values[index < WALK_STEPS ? index : WALK_STEPS - 1U];
}

static double walk_download(double t, unsigned int seed)
{
    static struct walk_cache cache;

    return walk(&cache, t, seed, 30.0, 90.0);
}

static double walk_upload(double t, unsigned int seed)
{
    static struct walk_cache cache;

    return walk(&cache, t, seed + 1000U, 6.0, 18.0);
}

static double asym_download(double t, unsigned int seed) { (void)t; (void)seed; return 200.0; }
static double asym_upload(double t, unsigned int seed) { (void)t; (void)seed; return 10.0; }

static double upload_step(double t, unsigned int seed)
{
    (void)seed;
    return t < 60.0 ? 15.0 : (t < 120.0 ? 6.0 : 15.0);
}

static double asym_walk_download(double t, unsigned int seed)
{
    static struct walk_cache cache;

    return walk(&cache, t, seed + 2000U, 150.0, 250.0);
}

static double asym_walk_upload(double t, unsigned int seed)
{
    static struct walk_cache cache;

    return walk(&cache, t, seed + 3000U, 4.0, 12.0);
}

static const struct scenario scenarios[] = {
    {
        "steady", 120.0, 1U,
        {
            { steady_download, 10.0, 100.0, 20000, 60000, 100000 },
            { steady_upload, 50.0, 100.0, 5000, 12000, 20000 }
        }
    },
    {
        "step-down", 180.0, 1U,
        {
            { step_download, 10.0, 170.0, 20000, 60000, 100000 },
            { steady_upload, 1000.0, 1000.0, 5000, 12000, 20000 }
        }
    },
    {
        "sine", 200.0, 1U,
        {
            { sine_download, 10.0, 190.0, 20000, 60000, 100000 },
            { sine_upload, 10.0, 190.0, 5000, 12000, 20000 }
        }
    },
    {
        "random-walk", 300.0, 5U,
        {
            { walk_download, 10.0, 290.0, 20000, 60000, 100000 },
            { walk_upload, 10.0, 290.0, 5000, 12000, 20000 }
        }
    },
    /* Asymmetric lines, where upload is the narrow, bloated direction. */
    {
        "asym-steady", 120.0, 1U,
        {
            { asym_download, 10.0, 110.0, 50000, 150000, 250000 },
            { asym_upload, 10.0, 110.0, 3000, 8000, 20000 }
        }
    },
    {
        "upload-step", 180.0, 1U,
        {
            { asym_download, 1000.0, 1000.0, 50000, 150000, 250000 },
            { upload_step, 10.0, 170.0, 3000, 8000, 20000 }
        }
    },
    {
        "asym-walk", 300.0, 5U,
        {
            { asym_walk_download, 10.0, 290.0, 50000, 150000, 250000 },
            { asym_walk_upload, 10.0, 290.0, 3000, 8000, 20000 }
        }
    }
};

/* cake-autorate ac75f49 defaults, as cake-adapt ships them. */
static uint64_t drain_period_microseconds;
static uint64_t adjust_up_microseconds = 10000U;
static uint64_t delay_threshold_microseconds = 30000U;
static uint64_t adjust_down_microseconds = 60000U;
static bool attribute_download;
/* Measured idle OWD deltas (microseconds), replayed as measurement noise. */
static double *noise_us;
static size_t noise_count;
static double noise_scale = 1.0;

static void load_noise(const char *path)
{
    FILE *file = fopen(path, "r");
    size_t capacity = 0U;
    double value;

    if (file == NULL) {
        perror(path);
        exit(2);
    }
    while (fscanf(file, "%lf", &value) == 1) {
        if (noise_count == capacity) {
            capacity = capacity == 0U ? 1024U : capacity * 2U;
            noise_us = realloc(noise_us, capacity * sizeof(*noise_us));
            if (noise_us == NULL) {
                perror("realloc");
                exit(1);
            }
        }
        noise_us[noise_count++] = value;
    }
    (void)fclose(file);
}

/* Jitter in milliseconds: a recorded idle delta, or uniform 0-2 ms without one. */
static double jitter_ms(uint32_t *state)
{
    if (noise_count == 0U) {
        return unit_random(state) * 2.0;
    }
    return noise_us[next_random(state) % noise_count] * noise_scale / 1000.0;
}

static struct controller_config controller_config(const struct scenario *scenario)
{
    struct controller_config config = {
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
        .decay_refractory_period_microseconds = 1000000U,
        .queue_drain_period_microseconds = drain_period_microseconds,
        .shared_delay = attribute_download
    };
    struct controller_direction_config *directions[2] = { &config.download, &config.upload };
    unsigned int index;

    for (index = 0U; index < 2U; index++) {
        const struct direction_profile *profile = &scenario->direction[index];

        *directions[index] = (struct controller_direction_config) {
            .adjust = true,
            .minimum_rate_bits_per_second = profile->minimum_mbps_x1000 * KILOBIT,
            .base_rate_bits_per_second = profile->base_mbps_x1000 * KILOBIT,
            .maximum_rate_bits_per_second = profile->maximum_mbps_x1000 * KILOBIT,
            .average_delay_maximum_adjust_up_microseconds = adjust_up_microseconds,
            .delay_threshold_microseconds = delay_threshold_microseconds,
            .average_delay_maximum_adjust_down_microseconds = adjust_down_microseconds
        };
    }
    return config;
}

static void record(struct metrics *metrics, double value)
{
    if (metrics->count == metrics->capacity) {
        metrics->capacity = metrics->capacity == 0U ? 4096U : metrics->capacity * 2U;
        metrics->delays_ms = realloc(metrics->delays_ms, metrics->capacity * sizeof(double));
        if (metrics->delays_ms == NULL) {
            perror("realloc");
            exit(1);
        }
    }
    metrics->delays_ms[metrics->count++] = value;
}

static int compare_doubles(const void *first, const void *second)
{
    double a = *(const double *)first;
    double b = *(const double *)second;

    return (a > b) - (a < b);
}

static double percentile(const struct metrics *metrics, double fraction)
{
    size_t index = (size_t)(fraction * (double)(metrics->count - 1U));

    return metrics->delays_ms[index];
}

static double share_above(const struct metrics *metrics, double limit_ms)
{
    size_t index;
    size_t above = 0U;

    for (index = 0U; index < metrics->count; index++) {
        above += metrics->delays_ms[index] > limit_ms;
    }
    return 100.0 * (double)above / (double)metrics->count;
}

static bool loaded(const struct direction_profile *profile, double seconds)
{
    return seconds >= profile->load_start_seconds && seconds < profile->load_end_seconds;
}

static double queue_delay_ms(const struct bottleneck *link, double capacity_bps)
{
    return link->queue_bytes * 8.0 / capacity_bps * 1000.0;
}

static void run(
    const struct scenario *scenario,
    unsigned int seed,
    bool one_way,
    bool trace,
    struct metrics metrics[2]
)
{
    struct controller_config config = controller_config(scenario);
    const struct latency_tracker_config tracker_config = {
        .alpha_baseline_increase_per_million = 1000U,
        .alpha_baseline_decrease_per_million = 900000U,
        .alpha_delta_ewma_per_million = 95000U
    };
    struct controller controller;
    struct latency_tracker trackers[REFLECTORS];
    struct bottleneck links[2];
    double base_owd_ms[REFLECTORS];
    double cake_bytes_at_sample[2] = { 0.0, 0.0 };
    uint64_t traffic_rate[2] = { 0U, 0U };
    uint64_t sample_id = 0U;
    uint64_t now;
    uint64_t end = (uint64_t)(scenario->duration_seconds * 1e6);
    uint32_t state = 12345U + seed;
    unsigned int reply = 0U;
    unsigned int index;

    memset(links, 0, sizeof(links));
    if (controller_init(&controller, &config) != 0) {
        fprintf(stderr, "controller_init failed\n");
        exit(1);
    }
    for (index = 0U; index < REFLECTORS; index++) {
        (void)tracker_init(&trackers[index], &tracker_config);
        base_owd_ms[index] = 5.0 + 4.0 * (double)index;
    }
    for (index = 0U; index < 2U; index++) {
        double peak = 0.0;
        double t;

        for (t = 0.0; t < scenario->duration_seconds; t += 0.1) {
            double c = scenario->direction[index].capacity_mbps(t, seed);
            peak = c > peak ? c : peak;
        }
        links[index].buffer_bytes = peak * 1e6 * BLOAT_SECONDS / 8.0;
    }
    controller_set_serialization_compensation(
        &controller,
        WIRE_PACKET_BITS,
        WIRE_PACKET_BITS,
        config.download.base_rate_bits_per_second,
        config.upload.base_rate_bits_per_second
    );
    if (trace) {
        printf("seconds,dl_capacity_mbps,dl_shaper_mbps,dl_achieved_mbps,dl_queue_ms,"
               "ul_capacity_mbps,ul_shaper_mbps,ul_achieved_mbps,ul_queue_ms\n");
    }

    for (now = 0U; now < end; now += STEP_US) {
        double seconds = (double)now / 1e6;
        double capacity[2];
        double delay_ms[2];
        const struct controller_direction *directions[2] = { &controller.download, &controller.upload };

        for (index = 0U; index < 2U; index++) {
            const struct direction_profile *profile = &scenario->direction[index];
            struct bottleneck *link = &links[index];
            double shaper = (double)(directions[index]->shaper_rate_bits_per_second != 0U
                ? directions[index]->shaper_rate_bits_per_second
                : profile->base_mbps_x1000 * KILOBIT);
            double offered = loaded(profile, seconds) ? shaper : IDLE_RATE_BPS;
            double arrival;
            double served;

            capacity[index] = profile->capacity_mbps(seconds, seed) * 1e6;
            arrival = offered < shaper ? offered : shaper;
            /* A full buffer drops; TCP then sends no faster than the bottleneck drains. */
            if (link->queue_bytes >= link->buffer_bytes && arrival > capacity[index]) {
                arrival = capacity[index];
            }
            link->queue_bytes += arrival * 1e-3 / 8.0;
            link->cake_bytes += arrival * 1e-3 / 8.0;
            served = capacity[index] * 1e-3 / 8.0;
            served = served < link->queue_bytes ? served : link->queue_bytes;
            link->queue_bytes -= served;
            if (index == DL) {
                /* Ingress CAKE counts what the bottleneck delivers. */
                link->cake_bytes += served - arrival * 1e-3 / 8.0;
            }
            delay_ms[index] = queue_delay_ms(link, capacity[index]);
            if (loaded(profile, seconds)) {
                metrics[index].delivered_bits += served * 8.0;
                metrics[index].capacity_bits += capacity[index] * 1e-3;
                if (now % METRIC_SPACING_US == 0U) {
                    record(&metrics[index], delay_ms[index]);
                }
            }
        }

        if (now % TRAFFIC_INTERVAL_US == 0U && now > 0U) {
            sample_id++;
            for (index = 0U; index < 2U; index++) {
                traffic_rate[index] = (uint64_t)(
                    (links[index].cake_bytes - cake_bytes_at_sample[index]) * 8.0 /
                    ((double)TRAFFIC_INTERVAL_US / 1e6)
                );
                cake_bytes_at_sample[index] = links[index].cake_bytes;
            }
        }

        if (now % REPLY_SPACING_US == 0U && sample_id > 0U) {
            unsigned int reflector = reply++ % REFLECTORS;
            struct latency_sample sample = { 0 };
            struct latency_observation observation;
            struct controller_input input;
            struct controller_output output;
            double jitter_dl = jitter_ms(&state);
            double jitter_ul = jitter_ms(&state);
            double dl_ms = base_owd_ms[reflector] + delay_ms[DL] + jitter_dl;
            double ul_ms = base_owd_ms[reflector] + delay_ms[UL] + jitter_ul;

            if (!one_way) {
                dl_ms = ul_ms = (dl_ms + ul_ms) / 2.0;
            }
            sample.download_owd_microseconds = (int64_t)(dl_ms * 1000.0);
            sample.upload_owd_microseconds = (int64_t)(ul_ms * 1000.0);
            sample.timestamp_microseconds = now;
            tracker_update(&trackers[reflector], &sample, &observation);
            input = (struct controller_input) {
                .download = {
                    .valid = true,
                    .traffic_sample_id = sample_id,
                    .traffic_rate_bits_per_second = traffic_rate[DL],
                    .cake_rate_bits_per_second = controller.download.shaper_rate_bits_per_second
                        ? controller.download.shaper_rate_bits_per_second
                        : config.download.base_rate_bits_per_second
                },
                .upload = {
                    .valid = true,
                    .traffic_sample_id = sample_id,
                    .traffic_rate_bits_per_second = traffic_rate[UL],
                    .cake_rate_bits_per_second = controller.upload.shaper_rate_bits_per_second
                        ? controller.upload.shaper_rate_bits_per_second
                        : config.upload.base_rate_bits_per_second
                },
                .download_latency = { .valid = true, .owd_delta_microseconds = observation.download_owd_delta_microseconds },
                .upload_latency = { .valid = true, .owd_delta_microseconds = observation.upload_owd_delta_microseconds },
                .timestamp_microseconds = now
            };
            controller_update(&controller, &input, &output);
            controller_set_serialization_compensation(
                &controller,
                WIRE_PACKET_BITS,
                WIRE_PACKET_BITS,
                controller.download.shaper_rate_bits_per_second,
                controller.upload.shaper_rate_bits_per_second
            );
        }

        if (trace && now % 100000U == 0U) {
            printf("%.1f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%.1f\n",
                seconds,
                capacity[DL] / 1e6, (double)controller.download.shaper_rate_bits_per_second / 1e6,
                (double)traffic_rate[DL] / 1e6, delay_ms[DL],
                capacity[UL] / 1e6, (double)controller.upload.shaper_rate_bits_per_second / 1e6,
                (double)traffic_rate[UL] / 1e6, delay_ms[UL]);
        }
    }
    controller_close(&controller);
}

int main(int argc, char **argv)
{
    static const char *const names[2] = { "download", "upload" };
    bool one_way = false;
    const char *trace = NULL;
    size_t index;
    int argument;

    for (argument = 1; argument < argc; argument++) {
        if (strcmp(argv[argument], "--owd") == 0) {
            one_way = true;
        } else if (strcmp(argv[argument], "--up-thr") == 0 && argument + 1 < argc) {
            adjust_up_microseconds = strtoull(argv[++argument], NULL, 10) * 1000U;
        } else if (strcmp(argv[argument], "--delay-thr") == 0 && argument + 1 < argc) {
            delay_threshold_microseconds = strtoull(argv[++argument], NULL, 10) * 1000U;
        } else if (strcmp(argv[argument], "--down-thr") == 0 && argument + 1 < argc) {
            adjust_down_microseconds = strtoull(argv[++argument], NULL, 10) * 1000U;
        } else if (strcmp(argv[argument], "--noise") == 0 && argument + 1 < argc) {
            load_noise(argv[++argument]);
        } else if (strcmp(argv[argument], "--noise-scale") == 0 && argument + 1 < argc) {
            noise_scale = strtod(argv[++argument], NULL);
        } else if (strcmp(argv[argument], "--attribute") == 0) {
            attribute_download = true;
        } else if (strcmp(argv[argument], "--drain") == 0 && argument + 1 < argc) {
            drain_period_microseconds = strtoull(argv[++argument], NULL, 10) * 1000U;
        } else if (strcmp(argv[argument], "--trace") == 0 && argument + 1 < argc) {
            trace = argv[++argument];
        } else {
            fprintf(stderr, "usage: %s [--owd] [--drain MS] [--attribute] [--trace SCENARIO]\n", argv[0]);
            return 2;
        }
    }

    if (trace == NULL) {
        printf("delay mode: %s; bottleneck buffer %.0f ms at peak capacity; drain %" PRIu64 " ms\n",
            one_way ? "one-way (fping-ts/irtt)" : "RTT/2 (fping)", BLOAT_SECONDS * 1000.0,
            drain_period_microseconds / 1000U);
        printf("%-12s %-8s %8s %8s %8s %8s %8s %8s %8s\n",
            "scenario", "dir", "p50 ms", "p95 ms", "p99 ms", "max ms", ">15ms %", ">30ms %", "used %");
    }
    for (index = 0U; index < sizeof(scenarios) / sizeof(scenarios[0]); index++) {
        const struct scenario *scenario = &scenarios[index];
        struct metrics metrics[2];
        unsigned int seed;
        unsigned int direction;

        if (trace != NULL && strcmp(trace, scenario->name) != 0) {
            continue;
        }
        memset(metrics, 0, sizeof(metrics));
        for (seed = 0U; seed < scenario->seeds; seed++) {
            run(scenario, seed, one_way, trace != NULL, metrics);
            if (trace != NULL) {
                break;
            }
        }
        if (trace != NULL) {
            continue;
        }
        for (direction = 0U; direction < 2U; direction++) {
            struct metrics *m = &metrics[direction];

            if (m->count == 0U) {
                continue;
            }
            qsort(m->delays_ms, m->count, sizeof(double), compare_doubles);
            printf("%-12s %-8s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f\n",
                scenario->name, names[direction],
                percentile(m, 0.50), percentile(m, 0.95), percentile(m, 0.99),
                m->delays_ms[m->count - 1U],
                share_above(m, 15.0), share_above(m, 30.0),
                100.0 * m->delivered_bits / m->capacity_bits);
        }
        free(metrics[0].delays_ms);
        free(metrics[1].delays_ms);
    }
    return 0;
}
