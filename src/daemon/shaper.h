#ifndef DAEMON_SHAPER_H_INCLUDED
#define DAEMON_SHAPER_H_INCLUDED

/* shaper.c: drives the CAKE shapers with the controller's decisions: its input, and bandwidth changes read back. */

#include "controller/controller.h"
#include "latency/parser.h"
#include "latency/tracker.h"

#include <stdbool.h>
#include <stdint.h>

struct daemon;

/* One ping reply's pass through the controller. */
struct shaper_step {
	const struct latency_observation *latency;
	const struct latency_sample *sample;
	struct controller_input input;
	struct controller_output output;
};

struct daemon_shaper {
	struct controller controller;
	bool initial_shaper_reported;
};

int shaper_start(struct daemon *daemon);

void shaper_update_compensation(struct daemon *daemon);

bool shaper_low_load(const struct daemon *daemon);

void shaper_update(
	struct daemon *daemon,
	const struct latency_observation *latency,
	const struct latency_sample *sample
);

void shaper_enforce_minimum(struct daemon *daemon, uint64_t timestamp_us);

#endif
