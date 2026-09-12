#ifndef SQM_MON_CONTROLLER_H
#define SQM_MON_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

enum controller_line_state {
    CONTROLLER_LINE_UNKNOWN,
    CONTROLLER_LINE_BELOW_CAPACITY,
    CONTROLLER_LINE_SATURATED
};

struct controller_direction_input {
    bool valid;
    uint64_t traffic_rate_bits_per_second;
    uint64_t cake_rate_bits_per_second;
};

struct controller_input {
    struct controller_direction_input download;
    struct controller_direction_input upload;
};

struct controller_output {
    enum controller_line_state download_state;
    enum controller_line_state upload_state;
    bool download_state_changed;
    bool upload_state_changed;
};

struct controller_direction {
    enum controller_line_state state;
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
