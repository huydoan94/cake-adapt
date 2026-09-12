#ifndef SQM_MON_CONTROLLER_H
#define SQM_MON_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

enum controller_line_state {
    CONTROLLER_LINE_UNKNOWN,
    CONTROLLER_LINE_BELOW_CAPACITY,
    CONTROLLER_LINE_SATURATED
};

enum controller_congestion_state {
    CONTROLLER_CONGESTION_UNKNOWN,
    CONTROLLER_CONGESTION_CLEAR,
    CONTROLLER_CONGESTION_DETECTED
};

struct controller_direction_input {
    bool valid;
    uint64_t traffic_rate_bits_per_second;
    uint64_t cake_rate_bits_per_second;
};

struct controller_latency_input {
    bool valid;
    uint32_t current_rtt_microseconds;
    uint32_t baseline_rtt_microseconds;
};

struct controller_input {
    struct controller_direction_input download;
    struct controller_direction_input upload;
    struct controller_latency_input latency;
};

struct controller_output {
    enum controller_line_state download_state;
    enum controller_line_state upload_state;
    enum controller_congestion_state download_congestion;
    enum controller_congestion_state upload_congestion;
    bool download_state_changed;
    bool upload_state_changed;
    bool download_congestion_changed;
    bool upload_congestion_changed;
};

struct controller_direction {
    enum controller_line_state state;
    enum controller_congestion_state congestion;
    unsigned int saturation_samples;
    unsigned int recovery_samples;
};

struct sqm_mon_controller {
    struct controller_direction download;
    struct controller_direction upload;
};

void controller_init(struct sqm_mon_controller *controller);

void controller_update(
    struct sqm_mon_controller *controller,
    const struct controller_input *input,
    struct controller_output *output
);

#endif
