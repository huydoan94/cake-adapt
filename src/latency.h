#ifndef LATENCY_H_INCLUDED
#define LATENCY_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define LATENCY_OUTPUT_SIZE 512U
#define LATENCY_TARGET_SIZE 256U

bool target_is_valid(const char *target);

struct latency {
    int output_descriptor;
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
    uint32_t one_way_microseconds;
    uint32_t one_way_baseline_microseconds;
    int64_t one_way_delta_microseconds;
    int64_t one_way_delta_ewma_microseconds;
    uint64_t timestamp_microseconds;
    uint64_t sequence;
};

enum latency_fping_line_result {
    LATENCY_FPING_LINE_SAMPLE,
    LATENCY_FPING_LINE_TIMEOUT,
    LATENCY_FPING_LINE_INVALID
};

struct latency_tracker_config {
    uint64_t alpha_baseline_increase_per_million;
    uint64_t alpha_baseline_decrease_per_million;
    uint64_t alpha_delta_ewma_per_million;
};

struct latency_tracker {
    struct latency_tracker_config config;
    uint32_t one_way_baseline_microseconds;
    int64_t one_way_delta_ewma_microseconds;
};

struct reflector_health_config {
    uint64_t response_deadline_microseconds;
    size_t detection_window;
    size_t detection_threshold;
};

struct reflector_health {
    struct reflector_health_config config;
    unsigned char *offences;
    uint64_t last_response_microseconds;
    size_t offence_index;
    size_t offence_count;
};

enum reflector_health_result {
    REFLECTOR_HEALTHY,
    REFLECTOR_OFFENCE,
    REFLECTOR_MISBEHAVING
};

struct reflector_comparison {
    uint64_t minimum_sum_owd_baselines_microseconds;
    uint64_t sum_owd_baselines_microseconds;
    uint64_t sum_owd_baselines_delta_microseconds;
    int64_t minimum_download_delta_ewma_microseconds;
    int64_t download_delta_ewma_microseconds;
    int64_t download_delta_ewma_delta_microseconds;
    int64_t minimum_upload_delta_ewma_microseconds;
    int64_t upload_delta_ewma_microseconds;
    int64_t upload_delta_ewma_delta_microseconds;
};

void latency_init(struct latency *latency);

bool latency_is_open(const struct latency *latency);

int latency_open(
    struct latency *latency,
    const char *interface,
    const char *const *targets,
    size_t target_count,
    uint64_t reflector_ping_interval_microseconds,
    const char *extra_arguments,
    const char *prefix,
    char *error,
    size_t error_size
);

void latency_close(struct latency *latency);

enum latency_fping_line_result parse_fping_line(
    const char *line,
    struct latency_sample *sample
);

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

int health_init(
    struct reflector_health *health,
    const struct reflector_health_config *config,
    uint64_t start_microseconds
);

void health_cleanup(struct reflector_health *health);

void health_reset(
    struct reflector_health *health,
    uint64_t start_microseconds
);

void health_record_response(
    struct reflector_health *health,
    uint64_t timestamp_microseconds
);

enum reflector_health_result health_check(
    struct reflector_health *health,
    uint64_t timestamp_microseconds
);

/* Validated, nonempty active order; indices refer to initialized trackers. */
void reflector_compare(
    const struct latency_tracker *trackers,
    const size_t *reflector_order,
    size_t active_count,
    struct reflector_comparison *comparisons
);

/* Caller supplies an active pinger and at least one standby reflector. */
void reflector_rotate(
    size_t *reflector_order,
    size_t reflector_count,
    size_t active_count,
    size_t pinger
);

enum latency_probe_result latency_receive(
    struct latency *latency,
    struct latency_sample *sample,
    char *error,
    size_t error_size
);

#endif
