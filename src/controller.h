#ifndef CONTROLLER_H_INCLUDED
#define CONTROLLER_H_INCLUDED

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
    uint64_t average_delay_maximum_adjust_up_microseconds;
    uint64_t delay_threshold_microseconds;
    uint64_t average_delay_maximum_adjust_down_microseconds;
};

struct controller_config {
    struct controller_direction_config download;
    struct controller_direction_config upload;
    unsigned int bufferbloat_detection_window;
    unsigned int bufferbloat_detection_threshold;
    uint64_t rate_minimum_adjust_down_bufferbloat_per_thousand;
    uint64_t rate_maximum_adjust_down_bufferbloat_per_thousand;
    uint64_t rate_minimum_adjust_up_high_load_per_thousand;
    uint64_t rate_maximum_adjust_up_high_load_per_thousand;
    uint64_t rate_adjust_down_low_load_per_thousand;
    uint64_t rate_adjust_up_low_load_per_thousand;
    uint64_t high_load_threshold_percent;
    uint64_t bufferbloat_refractory_period_microseconds;
    uint64_t decay_refractory_period_microseconds;
};

struct controller_direction_input {
    bool valid;
    /* Changes only after a new achieved-rate measurement; zero before the first. */
    uint64_t traffic_sample_id;
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

struct controller_direction_output {
    enum controller_line_state state;
    enum controller_congestion_state congestion;
    enum controller_rate_reason rate_reason;
    uint64_t rate_bits_per_second;
    int64_t delay_sum_microseconds;
    int64_t average_delay_microseconds;
    unsigned int delayed_sample_count;
    bool state_changed;
    bool congestion_changed;
    bool rate_changed;
};

struct controller_output {
    struct controller_direction_output download;
    struct controller_direction_output upload;
};

enum controller_activity_state {
    CONTROLLER_RUNNING,
    CONTROLLER_IDLE,
    CONTROLLER_STALL
};

struct controller_activity_config {
    bool enable_sleep;
    uint64_t active_threshold_bits_per_second;
    uint64_t stall_threshold_bits_per_second;
    uint64_t sustained_idle_microseconds;
    uint64_t stall_timeout_microseconds;
    uint64_t global_timeout_microseconds;
};

struct controller_activity_input {
    struct controller_direction_input download;
    struct controller_direction_input upload;
    uint64_t timestamp_microseconds;
    uint64_t last_response_microseconds;
    uint64_t last_pinger_start_microseconds;
    uint64_t grace_until_microseconds;
};

struct controller_activity {
    enum controller_activity_state state;
    uint64_t idle_started_microseconds;
    bool global_timeout_reported;
};

struct controller_activity_output {
    bool state_changed;
    bool check_stall_loads;
    bool global_timeout_started;
    bool restart_pingers;
};

struct controller_delay_sample {
    int64_t delay_microseconds;
    bool delayed;
};

struct controller_direction {
    struct controller_direction_config config;
    enum controller_line_state state;
    enum controller_congestion_state congestion;
    struct controller_delay_sample *delay_samples;
    int64_t delay_sum_microseconds;
    uint64_t shaper_rate_bits_per_second;
    unsigned int delay_next_sample;
    unsigned int delayed_sample_count;
    unsigned int saturation_samples;
    unsigned int recovery_samples;
    uint64_t last_congestion_adjustment_microseconds;
    uint64_t last_decay_adjustment_microseconds;
    uint64_t last_increase_sample_id;
    bool initial_rate_pending;
};

struct controller {
    struct controller_config config;
    struct controller_direction download;
    struct controller_direction upload;
};

int controller_init(
    struct controller *controller,
    const struct controller_config *config
);

void controller_close(struct controller *controller);

void controller_update(
    struct controller *controller,
    const struct controller_input *input,
    struct controller_output *output
);

void controller_set_minimum_rates(
    struct controller *controller,
    uint64_t timestamp_microseconds
);

void activity_update(
    struct controller_activity *activity,
    const struct controller_activity_config *config,
    const struct controller_activity_input *input,
    struct controller_activity_output *output
);

#endif
