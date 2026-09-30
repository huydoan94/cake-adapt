#ifndef LATENCY_H_INCLUDED
#define LATENCY_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "common/constants.h"

#define LATENCY_OUTPUT_SIZE 512U
#define LATENCY_TARGET_SIZE 256U

enum latency_backend {
    LATENCY_BACKEND_FPING,
    LATENCY_BACKEND_IRTT
};

bool target_is_valid(const char *target);

struct latency_child {
    int output_descriptor;
    pid_t process_identifier;
    uint64_t started_microseconds;
    uint64_t next_start_microseconds;
    /* Borrowed from the validated configuration for the session lifetime. */
    const char *target;
    char output_buffer[LATENCY_OUTPUT_SIZE];
    size_t output_length;
};

struct latency {
    enum latency_backend backend;
    bool active;
    uint64_t irtt_session_duration_minutes;
    uint64_t reflector_ping_interval_microseconds;
    /* Borrowed from the validated configuration for the session lifetime. */
    const char *ping_extra_args;
    const char *ping_prefix_string;
    struct latency_child children[CONFIG_MAX_REFLECTORS];
    size_t child_count;
};

enum latency_probe_result {
    LATENCY_PROBE_SUCCESS,
    LATENCY_PROBE_TIMEOUT,
    LATENCY_PROBE_PENDING,
    LATENCY_PROBE_RESTART,
    LATENCY_PROBE_ERROR
};

struct latency_sample {
    char target[LATENCY_TARGET_SIZE];
    int64_t download_owd_microseconds;
    int64_t upload_owd_microseconds;
    uint64_t timestamp_microseconds;
    bool timestamp_rollover_sensitive;
    uint64_t sequence;
};

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

struct latency_direction_tracker {
    int64_t baseline_microseconds;
    int64_t delta_ewma_microseconds;
};

struct latency_tracker {
    struct latency_tracker_config config;
    struct latency_direction_tracker download;
    struct latency_direction_tracker upload;
};

void latency_init(struct latency *latency);

bool latency_is_open(const struct latency *latency);

size_t latency_child_count(const struct latency *latency);

int latency_child_descriptor(const struct latency *latency, size_t child_index);

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

int latency_open_irtt(
    struct latency *latency,
    const char *const *targets,
    size_t target_count,
    uint64_t reflector_ping_interval_microseconds,
    uint64_t session_duration_minutes,
    const char *extra_arguments,
    const char *prefix,
    uint64_t first_start_microseconds,
    char *error,
    size_t error_size
);

int latency_start_irtt_children(
    struct latency *latency,
    uint64_t timestamp_microseconds,
    char *error,
    size_t error_size
);

bool latency_irtt_start_pending(const struct latency *latency);

uint64_t latency_irtt_next_start_microseconds(const struct latency *latency);

enum latency_probe_result latency_receive_child(
    struct latency *latency,
    size_t child_index,
    struct latency_sample *sample,
    char *error,
    size_t error_size
);

enum latency_fping_line_result parse_fping_line(
    const char *line,
    struct latency_sample *sample
);

bool parse_irtt_line(
    const char *line,
    const char *target,
    uint64_t timestamp_microseconds,
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

#endif
