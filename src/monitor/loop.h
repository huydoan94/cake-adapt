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

/* One shaped link: its CAKE qdisc and the traffic rate read from it. */
struct monitor_direction {
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

/* links.c: both directions' CAKE and traffic, and qdisc lifecycle events. */
struct monitor_links {
	struct monitor_direction download;
	struct monitor_direction upload;
	struct uloop_fd qdisc_events;
	/* A qdisc event asked for an immediate traffic cycle. */
	bool qdisc_refresh;
	bool clock_failed;
	bool cadence_initialized;
	bool cadence_applied;
	uint64_t cadence_microseconds;
};

/* control.c */
struct monitor_control {
	struct controller controller;
	bool initial_shaper_reported;
};

struct monitor;

/* One pinger child: uloop drives both its output stream and its exit. */
struct pinger_watch {
	struct ustream_fd output;
	struct uloop_process process;
	struct monitor *monitor;
	size_t child_index;
	bool reading;
};

/* pingers.c: the pinger session, its children and its restart timing. */
struct monitor_pingers {
	struct latency latency;
	struct pinger_watch watches[CONFIG_MAX_REFLECTORS];
	struct uloop_timeout stop_timer;
	struct uloop_timeout failure_timer;
	struct uloop_timeout start_timer;
	bool observation_failed;
	bool response_clock_failed;
	/* Stopped while CAKE was missing; restarted with setup grace. */
	bool suspended;
	uint64_t last_response_microseconds;
	uint64_t last_restart_microseconds;
	/* IRTT sessions start aligned to ping slots counted from here. */
	uint64_t slot_origin_microseconds;
	uint64_t next_attempt_microseconds;
	uint64_t grace_until_microseconds;
};

/* reflectors.c: per-reflector latency trackers, active order and health. */
struct monitor_reflectors {
	struct latency_tracker trackers[CONFIG_MAX_REFLECTORS];
	/* Indexed by pinger slot; order maps a slot to its reflector. */
	struct reflector_health health[CONFIG_MAX_REFLECTORS];
	size_t order[CONFIG_MAX_REFLECTORS];
	struct uloop_interval health_timer;
	bool clock_failed;
	uint64_t last_replacement_microseconds;
	uint64_t last_comparison_microseconds;
};

/* tcpdelay.c: the TCP queue estimate and the upload ACK rate. */
struct monitor_tcp {
	struct tcpdelay_capture capture;
	struct tcpdelay_estimator estimator;
	bool open;
	/* Upload interface whose capture failed; retried once it is recreated. */
	unsigned int failed_index;
	uint64_t dropped_records;
	uint64_t next_counter_check_microseconds;
	/* Pure-ACK and upload byte counters at the last rate sample, and the rates since. */
	bool ack_sampled;
	bool ack_rate_valid;
	uint64_t ack_bytes;
	uint64_t upload_bytes;
	uint64_t ack_sampled_microseconds;
	uint64_t ack_rate_bits_per_second;
	uint64_t upload_rate_bits_per_second;
};

/* The whole daemon state; each part above belongs to the file named on it. */
struct monitor {
	const struct config *config;
	/* Shared by links.c (discovery, events) and control.c (bandwidth changes). */
	struct netlink netlink;
	struct monitor_links links;
	struct monitor_control control;
	struct monitor_pingers pingers;
	struct monitor_reflectors reflectors;
	struct monitor_tcp tcp;
	/* monitor.c: the traffic tick, activity state, CPU and log upkeep. */
	struct controller_activity activity;
	struct uloop_interval traffic_timer;
	struct uloop_interval cpu_timer;
	struct uloop_interval log_timer;
	struct uloop_signal log_export_signal;
	struct uloop_signal log_reset_signal;
	struct cpu_monitor cpu_monitor;
	size_t cpu_count;
	bool cpu_observation_failed;
	int result;
};

/* links.c */

int links_watch_events(struct monitor *monitor);

void links_apply_cadence(struct monitor *monitor);

/* Both directions have a CAKE qdisc. */
bool links_ready(const struct monitor *monitor);

/* Both directions have the MTU and bandwidth serialization needs. */
bool links_wire_ready(const struct monitor *monitor);

void links_observe(struct monitor *monitor);

/* control.c */

int control_start(struct monitor *monitor);

void control_update_compensation(struct monitor *monitor);

bool control_low_load(const struct monitor *monitor);

void control_update(struct monitor *monitor, const struct latency_observation *latency,
		    const struct latency_sample *sample);

void control_enforce_minimum(struct monitor *monitor, uint64_t timestamp_microseconds);

/* tcpdelay.c */

void tcp_observe(struct monitor *monitor, uint64_t timestamp_microseconds,
		 struct controller_queue_input *queue, struct controller_ack_input *acks);

void tcp_close(struct monitor *monitor);

/* pingers.c */

void pingers_prepare(struct monitor *monitor, uint64_t start_microseconds);

void pingers_close(struct monitor *monitor);

void pingers_stop_now(struct monitor *monitor);

bool pingers_watch(struct monitor *monitor);

void pingers_grant_grace(struct monitor *monitor, uint64_t timestamp_microseconds);

void pingers_restart(struct monitor *monitor, uint64_t timestamp_microseconds);

/* reflectors.c */

int reflectors_start(struct monitor *monitor, uint64_t start_microseconds);

void reflectors_stop(struct monitor *monitor);

void reflectors_reset_health(struct monitor *monitor, uint64_t timestamp_microseconds);

/* monitor.c */

void monitor_tick(struct monitor *monitor);

#endif
