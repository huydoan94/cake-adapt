#ifndef LATENCY_TRACKER_H_INCLUDED
#define LATENCY_TRACKER_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "latency/parser.h"

struct latency_observation {
    int64_t download_owd_microseconds;
    int64_t download_owd_baseline_microseconds;
    int64_t download_owd_delta_microseconds;
    int64_t download_owd_delta_ewma_microseconds;
    int64_t upload_owd_microseconds;
    int64_t upload_owd_baseline_microseconds;
    int64_t upload_owd_delta_microseconds;
    int64_t upload_owd_delta_ewma_microseconds;
    uint64_t timestamp_microseconds;
    uint64_t sequence;
};

struct latency_tracker_config {
    uint64_t alpha_baseline_increase_per_million;
    uint64_t alpha_baseline_decrease_per_million;
    uint64_t alpha_delta_ewma_per_million;
};

struct latency_direction_tracker {
    int64_t baseline_microseconds;
    int64_t delta_ewma_microseconds;
};

struct latency_tracker {
    struct latency_tracker_config config;
    struct latency_direction_tracker download;
    struct latency_direction_tracker upload;
};

int tracker_init(
    struct latency_tracker *tracker,
    const struct latency_tracker_config *config
);

void tracker_reset(struct latency_tracker *tracker);

void tracker_update(
    struct latency_tracker *tracker,
    const struct latency_sample *sample,
    struct latency_observation *observation
);

void tracker_update_delta_ewma(
    struct latency_tracker *tracker,
    bool low_load,
    struct latency_observation *observation
);

#endif
