#ifndef LATENCY_H_INCLUDED
#define LATENCY_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "common/constants.h"
#include "latency/parser.h"

#define LATENCY_OUTPUT_SIZE 512U

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

#endif
