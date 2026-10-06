#ifndef CONTROLLER_H_INCLUDED
#define CONTROLLER_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

enum controller_line_state {
	CONTROLLER_LINE_UNKNOWN,
	CONTROLLER_LINE_BELOW_CAPACITY,
	CONTROLLER_LINE_SATURATED
};

/* cake-autorate's per-direction load class, as its DATA and SUMMARY records print it. */
enum controller_load {
	CONTROLLER_LOAD_IDLE,
	CONTROLLER_LOAD_LOW,
	CONTROLLER_LOAD_HIGH
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
	CONTROLLER_RATE_RECONCILE,
	/* Download held so its ACKs take no more than their share of upload. */
	CONTROLLER_RATE_ACK_SHARE
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
	/*
	 * Both directions report the same RTT/2 delay (fping), so a queue is
	 * attributed using download, which is shaped after the bottleneck: its
	 * achieved rate is what the bottleneck delivers.
	 */
	bool shared_delay;
	/*
	 * While upload is under high load, download is held so that its ACKs use
	 * no more than the upload left by other traffic (less a headroom), but
	 * never below this share of the upload shaper rate. Not a reservation:
	 * ACKs needing less leave the rest to other traffic. Zero disables it.
	 */
	uint64_t ul_congest_ack_share_percent;
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
	int64_t owd_delta_microseconds;
};

/* Queueing delay in each direction measured separately, from TCP timestamps. */
struct controller_queue_input {
	bool valid;
	int64_t download_microseconds;
	int64_t upload_microseconds;
};

/* Upload split into pure ACKs and all traffic, over the same window, after the shaper. */
struct controller_ack_input {
	bool valid;
	uint64_t upload_ack_rate_bits_per_second;
	uint64_t upload_rate_bits_per_second;
};

struct controller_input {
	struct controller_direction_input download;
	struct controller_direction_input upload;
	struct controller_latency_input download_latency;
	struct controller_latency_input upload_latency;
	/*
	 * Used only with shared_delay. When valid and large enough, it replaces
	 * the delivery heuristic and splits the round-trip delta between the
	 * directions by their measured shares.
	 */
	struct controller_queue_input queue;
	/* Used only with ul_congest_ack_share_percent. */
	struct controller_ack_input acks;
	uint64_t timestamp_microseconds;
};

struct controller_direction_output {
	enum controller_line_state state;
	enum controller_congestion_state congestion;
	enum controller_rate_reason rate_reason;
	uint64_t rate_bits_per_second;
	int64_t average_delay_microseconds;
	unsigned int delayed_sample_count;
	/*
	 * Bufferbloat is blamed on this direction, so a detected bufferbloat may
	 * cut its rate. Always true unless one shared delay must be attributed.
	 */
	bool bufferbloat_attributed;
	bool state_changed;
	bool congestion_changed;
	bool rate_changed;
	bool bufferbloat_attribution_changed;
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
	struct controller_activity_config config;
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

struct controller_direction {
	struct controller_direction_config config;
	enum controller_line_state state;
	enum controller_congestion_state congestion;
	int64_t *delay_samples;
	unsigned char *delayed_samples;
	int64_t delay_sum_microseconds;
	/* Over the detection window; zero while latency is unknown. */
	int64_t average_delay_microseconds;
	uint64_t shaper_rate_bits_per_second;
	unsigned int delay_next_sample;
	unsigned int delayed_sample_count;
	unsigned int saturation_samples;
	unsigned int recovery_samples;
	uint64_t last_congestion_adjustment_microseconds;
	uint64_t last_decay_adjustment_microseconds;
	uint64_t last_increase_sample_id;
	bool initial_rate_pending;
	/* The last decision, for reporting changes. */
	bool bufferbloat_attributed;
};

struct controller {
	struct controller_config config;
	struct controller_direction download;
	struct controller_direction upload;
};

int controller_init(struct controller *controller, const struct controller_config *config);

void controller_close(struct controller *controller);

void controller_update(
	struct controller *controller,
	const struct controller_input *input,
	struct controller_output *output
);

/* Raises each direction's delay thresholds by a wire packet's time at its shaper rate. */
void controller_set_serialization_compensation(
	struct controller *controller,
	uint64_t download_wire_packet_bits,
	uint64_t upload_wire_packet_bits
);

void controller_set_minimum_rates(struct controller *controller, uint64_t timestamp_microseconds);

/*
 * High above the high-load threshold; otherwise low while traffic exceeds the
 * connection active threshold, else idle.
 */
enum controller_load controller_load(
	const struct controller *controller,
	const struct controller_direction_input *input,
	uint64_t active_threshold_bits_per_second
);

/*
 * Both directions strictly below the high-load threshold. cake-autorate gates
 * the delay EWMA on its last load percentage, which is 0 before the first
 * achieved-rate sample; an input without a CAKE rate also counts as 0.
 */
bool controller_low_load(
	const struct controller *controller,
	const struct controller_direction_input *download,
	const struct controller_direction_input *upload
);

/* Starts RUNNING under config. */
void activity_init(
	struct controller_activity *activity,
	const struct controller_activity_config *config
);

void activity_update(
	struct controller_activity *activity,
	const struct controller_activity_input *input,
	struct controller_activity_output *output
);

#endif
