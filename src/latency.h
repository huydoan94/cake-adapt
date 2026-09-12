#ifndef SQM_MON_LATENCY_H
#define SQM_MON_LATENCY_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define LATENCY_BASELINE_WINDOW_SAMPLES 300U

struct sqm_mon_latency {
    int socket_descriptor;
    struct sockaddr_in target_address;
    uint16_t identifier;
    uint16_t next_sequence;
};

enum latency_probe_result {
    LATENCY_PROBE_SUCCESS,
    LATENCY_PROBE_TIMEOUT,
    LATENCY_PROBE_ERROR
};

struct latency_sample {
    uint32_t round_trip_microseconds;
};

struct latency_observation {
    uint32_t round_trip_microseconds;
    uint32_t baseline_microseconds;
    uint32_t delta_microseconds;
};

struct latency_tracker {
    uint32_t samples[LATENCY_BASELINE_WINDOW_SAMPLES];
    size_t next_sample;
    size_t sample_count;
};

void latency_init(struct sqm_mon_latency *latency);

int latency_open(
    struct sqm_mon_latency *latency,
    const char *interface,
    const char *target,
    char *error,
    size_t error_size
);

void latency_close(struct sqm_mon_latency *latency);

void latency_tracker_init(struct latency_tracker *tracker);

void latency_tracker_update(
    struct latency_tracker *tracker,
    const struct latency_sample *sample,
    struct latency_observation *observation
);

enum latency_probe_result latency_probe(
    struct sqm_mon_latency *latency,
    int timeout_milliseconds,
    struct latency_sample *sample,
    char *error,
    size_t error_size
);

#endif
