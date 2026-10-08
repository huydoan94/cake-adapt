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
#include "tcpdelay/injector.h"

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
	uint64_t traffic_rate_bps;
	uint64_t traffic_sample_id;
	bool cake_valid;
	bool traffic_valid;
	uint64_t next_cake_observation_us;
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
	uint64_t cadence_us;
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
	uint64_t last_response_us;
	uint64_t last_restart_us;
	uint64_t next_attempt_us;
	uint64_t grace_until_us;
};

/* reflectors.c: per-reflector latency trackers, active order and health. */
/* A pinger slot's largest added round-trip delay in the current and previous second. */
struct reflector_recent_delay {
	uint64_t sec;
	int64_t current_us;
	int64_t previous_us;
};

struct monitor_reflectors {
	/* Shared by every health record below. */
	struct reflector_health_config health_config;
	struct latency_tracker trackers[CONFIG_MAX_REFLECTORS];
	/* Indexed by pinger slot; order maps a slot to its reflector. */
	struct reflector_health health[CONFIG_MAX_REFLECTORS];
	struct reflector_recent_delay recent[CONFIG_MAX_REFLECTORS];
	size_t order[CONFIG_MAX_REFLECTORS];
	struct uloop_interval health_timer;
	bool clock_failed;
	uint64_t last_replacement_us;
	uint64_t last_comparison_us;
};

/* tcpdelay.c: the TCP queue estimate, the upload ACK rate and timestamp injection. */
struct monitor_tcp {
	struct tcpdelay_capture capture;
	/* Attached while the capture is open, to the same interface. */
	struct tcpdelay_injector injector;
	/* Detaches the injector for a day when its handshakes stall repeatedly. */
	struct tcpdelay_inject_breaker inject_breaker;
	bool open;
	/* The upload CAKE the capture was opened for; another one needs a new capture. */
	struct qdisc_id qdisc;
	/* Upload interface whose capture failed; retried once it is recreated. */
	unsigned int failed_index;
	uint64_t dropped_records;
	uint64_t next_counter_check_us;
	/* Pure-ACK and upload byte counters at the last rate sample, and the rates since. */
	bool ack_sampled;
	bool ack_rate_valid;
	bool ack_degraded;
	uint64_t ack_bytes;
	uint64_t upload_bytes;
	uint64_t unaccounted_packets;
	uint64_t ack_sampled_us;
	uint64_t ack_rate_bps;
	uint64_t upload_rate_bps;
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
	/* monitor.c: the traffic tick, activity state, CPU, memory and log upkeep. */
	struct controller_activity activity;
	struct uloop_interval traffic_timer;
	struct uloop_interval cpu_timer;
	struct uloop_interval memory_timer;
	struct uloop_interval log_timer;
	struct uloop_signal log_export_signal;
	struct uloop_signal log_reset_signal;
	struct cpu_monitor cpu_monitor;
	size_t cpu_count;
	bool cpu_observation_failed;
	bool memory_observation_failed;
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

void control_update(
	struct monitor *monitor,
	const struct latency_observation *latency,
	const struct latency_sample *sample
);

void control_enforce_minimum(struct monitor *monitor, uint64_t timestamp_us);

/* tcpdelay.c */

/* The capture's unloaded state; first, so shutdown is safe from any point. */
void tcp_init(struct monitor *monitor);

/*
 * Loads the TCP filter when a TCP feature is enabled, before pingers start:
 * the kernel's verifier can take seconds on slow CPUs.
 */
void tcp_start(struct monitor *monitor);

void tcp_observe(struct monitor *monitor, struct controller_input *input);

/* Empties the ring buffer between ping replies; see tcpdelay.c. */
void tcp_drain(struct monitor *monitor);

void tcp_close(struct monitor *monitor);

/* Closes and unloads the capture. */
void tcp_stop(struct monitor *monitor);

/* pingers.c */

void pingers_prepare(struct monitor *monitor, uint64_t start_us);

void pingers_close(struct monitor *monitor);

void pingers_stop_now(struct monitor *monitor);

bool pingers_watch(struct monitor *monitor);

/* CAKE is missing: stop the pingers until pingers_unsuspend(). */
void pingers_suspend(struct monitor *monitor);

void pingers_unsuspend(struct monitor *monitor, uint64_t timestamp_us);

/* Leaving IDLE: start again with setup grace. */
void pingers_resume(struct monitor *monitor, uint64_t timestamp_us);

/* Restart now with the current reflectors, as after a rotation. */
void pingers_reopen(struct monitor *monitor);

void pingers_restart(struct monitor *monitor, uint64_t timestamp_us);

/* reflectors.c */

int reflectors_start(struct monitor *monitor, uint64_t start_us);

void reflectors_stop(struct monitor *monitor);

/* Fills targets with the reflectors in the no_pingers active slots, in slot order. */
void reflectors_active(const struct monitor *monitor, const char *targets[]);

/* The active pinger slot polling target, or SIZE_MAX when none does. */
size_t reflectors_find(const struct monitor *monitor, const char *target);

/*
 * Feeds one reply into its reflector's latency tracker, whose delay EWMA moves
 * only under low load, and into the slot's health record.
 */
void reflectors_record(
	struct monitor *monitor,
	size_t slot,
	const struct latency_sample *sample,
	uint64_t response_us,
	struct latency_observation *observation
);

void reflectors_reset_health(struct monitor *monitor, uint64_t timestamp_us);

/*
 * fping's recent added round-trip delay on the access link, or -1 when no
 * pinger slot replied in the current or previous second. A queue on the link
 * delays every reflector's replies, while a slow reflector delays only its own:
 * this is the lower median across slots of each slot's largest added delay over
 * those two seconds. One slow reflector cannot raise it, and one whose baseline
 * sits too high cannot hold it down.
 */
int64_t reflectors_recent_delay_us(const struct monitor *monitor, uint64_t timestamp_us);

/* monitor.c */

void monitor_tick(struct monitor *monitor);

#endif
