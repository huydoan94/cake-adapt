#ifndef MONITOR_LINKS_H_INCLUDED
#define MONITOR_LINKS_H_INCLUDED

/* links.c: both directions' CAKE and traffic, and qdisc lifecycle events. */

#include "cake/cake.h"
#include "platform/traffic.h"

#include <libubox/uloop.h>
#include <stdbool.h>
#include <stdint.h>

struct monitor;

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

int links_watch_events(struct monitor *monitor);

void links_apply_cadence(struct monitor *monitor);

/* Both directions have a CAKE qdisc. */
bool links_ready(const struct monitor *monitor);

/* Both directions have the MTU and bandwidth serialization needs. */
bool links_wire_ready(const struct monitor *monitor);

void links_observe(struct monitor *monitor);

#endif
