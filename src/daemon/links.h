#ifndef DAEMON_LINKS_H_INCLUDED
#define DAEMON_LINKS_H_INCLUDED

/* links.c: both directions' CAKE and traffic, and qdisc lifecycle events. */

#include "common/direction.h"
#include "cake/cake.h"
#include "platform/traffic.h"

#include <libubox/uloop.h>
#include <stdbool.h>
#include <stdint.h>

struct daemon;

enum links_cake_state {
	LINKS_CAKE_UNKNOWN,
	LINKS_CAKE_AVAILABLE,
	LINKS_CAKE_NOT_FOUND,
	LINKS_CAKE_FAILED
};

enum links_traffic_state {
	LINKS_TRAFFIC_UNKNOWN,
	LINKS_TRAFFIC_AVAILABLE,
	LINKS_TRAFFIC_UNAVAILABLE
};

/* One shaped link: its CAKE qdisc and the traffic rate read from it. */
struct links_direction {
	const char *name;
	const char *interface;
	struct traffic_monitor traffic_monitor;
	struct cake_observation cake;
	enum links_cake_state cake_state;
	enum links_traffic_state traffic_state;
	uint64_t traffic_rate_bps;
	uint64_t traffic_sample_id;
	bool cake_valid;
	bool traffic_valid;
	uint64_t next_cake_observation_us;
};

struct daemon_links {
	struct links_direction directions[DIRECTION_COUNT];
	struct uloop_fd qdisc_events;
	/* A qdisc event asked for an immediate traffic cycle. */
	bool qdisc_refresh;
	bool clock_failed;
	bool cadence_initialized;
	bool cadence_applied;
	uint64_t cadence_us;
};

int links_watch_events(struct daemon *daemon);

void links_apply_cadence(struct daemon *daemon);

/* Both directions have a CAKE qdisc. */
bool links_ready(const struct daemon *daemon);

/* Both directions have the MTU and bandwidth serialization needs. */
bool links_wire_ready(const struct daemon *daemon);

void links_observe(struct daemon *daemon);

#endif
