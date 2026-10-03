#ifndef MONITOR_LOOP_H_INCLUDED
#define MONITOR_LOOP_H_INCLUDED

/* Private to the monitor: event-loop state shared by its source files. */

#include "cake/cake.h"
#include "config/config.h"
#include "controller/controller.h"
#include "controller/reflector.h"
#include "latency/latency.h"
#include "latency/tracker.h"
#include "platform/cpu.h"
#include "platform/netlink.h"
#include "platform/traffic.h"
#include "tcpdelay/capture.h"
#include "tcpdelay/estimator.h"

#include <libubox/uloop.h>
#include <libubox/ustream.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum cake_observation_state {
	CAKE_OBSERVATION_UNKNOWN,
	CAKE_OBSERVATION_AVAILABLE,
	CAKE_OBSERVATION_NOT_FOUND,
	CAKE_OBSERVATION_FAILED
};

enum traffic_observation_state {
	TRAFFIC_OBSERVATION_UNKNOWN,
	TRAFFIC_OBSERVATION_AVAILABLE,
	TRAFFIC_OBSERVATION_UNAVAILABLE
};

struct monitored_direction {
	const char *name;
	const char *interface;
	struct traffic_monitor traffic_monitor;
	struct cake_observation cake;
	enum cake_observation_state cake_state;
	enum traffic_observation_state traffic_state;
	uint64_t traffic_rate_bits_per_second;
	uint64_t traffic_sample_id;
	bool cake_valid;
	bool traffic_valid;
	uint64_t next_cake_observation_microseconds;
};

struct observation_context {
	struct controller controller;
	struct latency latency;
	struct latency_tracker latency_trackers[CONFIG_MAX_REFLECTORS];
	struct reflector_health reflector_health[CONFIG_MAX_REFLECTORS];
	size_t reflector_order[CONFIG_MAX_REFLECTORS];
	struct netlink netlink;
	struct monitored_direction download;
	struct monitored_direction upload;
	bool latency_observation_failed;
	bool response_clock_failed;
	bool traffic_clock_failed;
	bool health_clock_failed;
	bool traffic_cadence_initialized;
	bool pingers_suspended;
	bool initial_shaper_reported;
	uint64_t traffic_cadence_microseconds;
	uint64_t last_reflector_replacement_microseconds;
	uint64_t last_reflector_comparison_microseconds;
	uint64_t last_reflector_response_microseconds;
	uint64_t last_pinger_restart_microseconds;
	uint64_t pinger_slot_origin_microseconds;
	uint64_t next_latency_attempt_microseconds;
	uint64_t pinger_grace_until_microseconds;
	struct controller_activity activity;
	struct tcpdelay_capture tcp_capture;
	struct tcpdelay_estimator tcp_estimator;
	bool tcp_capture_open;
	/* Upload interface whose capture failed; retried once it is recreated. */
	unsigned int tcp_capture_failed_index;
	uint64_t tcp_dropped_records;
	uint64_t next_tcp_counter_check_microseconds;
	/* Pure-ACK and upload byte counters at the last rate sample, and the rates since. */
	bool ack_sampled;
	bool ack_rate_valid;
	uint64_t ack_bytes;
	uint64_t upload_bytes;
	uint64_t ack_sampled_microseconds;
	uint64_t ack_rate_bits_per_second;
	uint64_t upload_rate_bits_per_second;
};

struct event_loop;

/* One pinger child: uloop drives both its output stream and its exit. */
struct pinger_watch {
	struct ustream_fd output;
	struct uloop_process process;
	struct event_loop *loop;
	size_t child_index;
	bool reading;
};

struct event_loop {
	struct observation_context observation;
	const struct config *config;
	struct uloop_interval traffic_timer;
	struct uloop_interval reflector_health_timer;
	struct uloop_interval cpu_timer;
	struct uloop_interval log_timer;
	struct uloop_signal log_export_signal;
	struct uloop_signal log_reset_signal;
	struct cpu_monitor cpu_monitor;
	size_t cpu_count;
	bool cpu_observation_failed;
	bool qdisc_refresh;
	bool traffic_cadence_applied;
	struct uloop_fd qdisc_events;
	struct pinger_watch pingers[CONFIG_MAX_REFLECTORS];
	struct uloop_timeout pinger_stop_timer;
	struct uloop_timeout latency_failure_timer;
	struct uloop_timeout latency_start_timer;
	int result;
};

/* observe.c: CAKE and traffic observation */

int watch_qdisc_events(struct event_loop *loop);

void apply_traffic_cadence(struct event_loop *loop);

bool cake_ready(const struct observation_context *context);

bool wire_metadata_ready(const struct observation_context *context);

void observe_traffic_cycle(struct observation_context *context, const struct config *config);

/* control.c: controller decisions and CAKE updates */

int start_controller(struct controller *controller, const struct config *config);

void update_serialization_compensation(struct observation_context *context);

bool direction_has_low_load(const struct monitored_direction *direction,
			    uint64_t high_load_threshold_percent);

void update_controller(struct observation_context *context, const struct config *config,
		       const struct latency_observation *latency,
		       const struct latency_sample *sample);

void enforce_minimum_rates(struct observation_context *context, const struct config *config,
			   uint64_t timestamp_microseconds);

/* tcpdelay.c: per-direction queues and the upload ACK rate from TCP */

void observe_tcp_capture(struct observation_context *context, const struct config *config,
			 uint64_t timestamp_microseconds, struct controller_queue_input *queue,
			 struct controller_ack_input *acks);

void close_tcp_delay(struct observation_context *context);

/* pingers.c: pinger sessions and latency samples */

void prepare_pingers(struct event_loop *loop);

void close_latency(struct event_loop *loop);

void stop_pingers_now(struct event_loop *loop);

bool watch_latency(struct event_loop *loop);

void grant_pinger_setup_grace(struct observation_context *context, const struct config *config,
			      uint64_t timestamp_microseconds);

void restart_latency(struct event_loop *loop, uint64_t timestamp_microseconds);

/* reflectors.c: reflector health and replacement */

int start_reflectors(struct observation_context *context, const struct config *config,
		     uint64_t start_microseconds);

void stop_reflectors(struct observation_context *context, const struct config *config);

void reset_reflector_health(struct observation_context *context, const struct config *config,
			    uint64_t timestamp_microseconds);

void handle_reflector_health_timer(struct uloop_interval *timer);

/* monitor.c: activity state and the traffic tick */

void handle_traffic_timer(struct uloop_interval *timer);

#endif
