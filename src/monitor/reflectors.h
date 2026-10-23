#ifndef MONITOR_REFLECTORS_H_INCLUDED
#define MONITOR_REFLECTORS_H_INCLUDED

/* reflectors.c: per-reflector latency trackers, active order and health. */

#include "controller/reflector.h"
#include "latency/parser.h"
#include "latency/tracker.h"
#include "constants/options.h"

#include <libubox/uloop.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct monitor;

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
 * The echo pinger's recent added round-trip delay on the access link, or -1 when no
 * pinger slot replied in the current or previous second. A queue on the link
 * delays every reflector's replies, while a slow reflector delays only its own:
 * this is the lower median across slots of each slot's largest added delay over
 * those two seconds. One slow reflector cannot raise it, and one whose baseline
 * sits too high cannot hold it down.
 */
int64_t reflectors_recent_delay_us(const struct monitor *monitor, uint64_t timestamp_us);

#endif
