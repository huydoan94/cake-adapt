#ifndef SQM_MON_LATENCY_H
#define SQM_MON_LATENCY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define LATENCY_OUTPUT_SIZE 512U
#define LATENCY_TARGET_SIZE 256U

struct sqm_mon_latency {
    int output_descriptor;
    int diagnostic_descriptor;
    pid_t process_identifier;
    char output_buffer[LATENCY_OUTPUT_SIZE];
    size_t output_length;
};

enum latency_probe_result {
    LATENCY_PROBE_SUCCESS,
    LATENCY_PROBE_TIMEOUT,
    LATENCY_PROBE_PENDING,
    LATENCY_PROBE_ERROR
};

struct latency_sample {
    char target[LATENCY_TARGET_SIZE];
    uint32_t round_trip_microseconds;
    uint64_t timestamp_microseconds;
    uint64_t sequence;
};

struct latency_observation {
    uint32_t round_trip_microseconds;
    uint32_t baseline_microseconds;
    int64_t delta_microseconds;
    int64_t delta_ewma_microseconds;
    uint64_t timestamp_microseconds;
    uint64_t sequence;
};

enum latency_fping_line_result {
    LATENCY_FPING_LINE_SAMPLE,
    LATENCY_FPING_LINE_TIMEOUT,
    LATENCY_FPING_LINE_INVALID
};

struct latency_tracker {
    uint64_t baseline_scaled;
    int64_t delta_ewma_microseconds;
    bool initialized;
};

void latency_init(struct sqm_mon_latency *latency);

bool latency_is_open(const struct sqm_mon_latency *latency);

int latency_open(
    struct sqm_mon_latency *latency,
    const char *interface,
    const char *const *targets,
    size_t target_count,
    uint64_t reflector_ping_interval_microseconds,
    char *error,
    size_t error_size
);

void latency_close(struct sqm_mon_latency *latency);

enum latency_fping_line_result latency_parse_fping_line(
    const char *line,
    struct latency_sample *sample
);

void latency_tracker_init(struct latency_tracker *tracker);

void latency_tracker_update(
    struct latency_tracker *tracker,
    const struct latency_sample *sample,
    struct latency_observation *observation
);

void latency_tracker_update_delta_ewma(
    struct latency_tracker *tracker,
    bool low_load,
    struct latency_observation *observation
);

enum latency_probe_result latency_receive(
    struct sqm_mon_latency *latency,
    struct latency_sample *sample,
    char *error,
    size_t error_size
);

#endif
