#ifndef SQM_MON_CONTROLLER_H
#define SQM_MON_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#define CONTROLLER_DELAY_WINDOW_SAMPLES 6U
#define CONTROLLER_HIGH_LOAD_PERCENT 75U
#define CONTROLLER_OWD_DELAY_THRESHOLD_MICROSECONDS 30000U
#define CONTROLLER_OWD_MAXIMUM_ADJUST_UP_MICROSECONDS 10000U
#define CONTROLLER_OWD_MAXIMUM_ADJUST_DOWN_MICROSECONDS 60000U

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

enum controller_rate_reason {
    CONTROLLER_RATE_UNCHANGED,
    CONTROLLER_RATE_INITIAL,
    CONTROLLER_RATE_CONGESTION,
    CONTROLLER_RATE_HIGH_LOAD,
    CONTROLLER_RATE_RETURN_TO_BASE,
    CONTROLLER_RATE_RECONCILE
};

struct controller_direction_config {
    bool adjust;
    uint64_t minimum_rate_bits_per_second;
    uint64_t base_rate_bits_per_second;
    uint64_t maximum_rate_bits_per_second;
};

struct controller_config {
    struct controller_direction_config download;
    struct controller_direction_config upload;
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
    uint64_t timestamp_microseconds;
};

struct controller_output {
    enum controller_line_state download_state;
    enum controller_line_state upload_state;
    enum controller_congestion_state download_congestion;
    enum controller_congestion_state upload_congestion;
    enum controller_rate_reason download_rate_reason;
    enum controller_rate_reason upload_rate_reason;
    uint64_t download_rate_bits_per_second;
    uint64_t upload_rate_bits_per_second;
    uint64_t download_delay_sum_microseconds;
    uint64_t upload_delay_sum_microseconds;
    uint32_t download_average_delay_microseconds;
    uint32_t upload_average_delay_microseconds;
    unsigned int download_delayed_sample_count;
    unsigned int upload_delayed_sample_count;
    bool download_state_changed;
    bool upload_state_changed;
    bool download_congestion_changed;
    bool upload_congestion_changed;
    bool download_rate_changed;
    bool upload_rate_changed;
};

struct controller_direction {
    struct controller_direction_config config;
    enum controller_line_state state;
    enum controller_congestion_state congestion;
    uint32_t delay_samples[CONTROLLER_DELAY_WINDOW_SAMPLES];
    bool delayed_samples[CONTROLLER_DELAY_WINDOW_SAMPLES];
    uint64_t delay_sum_microseconds;
    uint64_t shaper_rate_bits_per_second;
    unsigned int delay_next_sample;
    unsigned int delayed_sample_count;
    unsigned int saturation_samples;
    unsigned int recovery_samples;
    uint64_t last_congestion_adjustment_microseconds;
    uint64_t last_decay_adjustment_microseconds;
    bool initial_rate_pending;
};

struct sqm_mon_controller {
    struct controller_direction download;
    struct controller_direction upload;
};

void controller_init(
    struct sqm_mon_controller *controller,
    const struct controller_config *config
);

void controller_update(
    struct sqm_mon_controller *controller,
    const struct controller_input *input,
    struct controller_output *output
);

#endif
