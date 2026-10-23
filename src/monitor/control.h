#ifndef MONITOR_CONTROL_H_INCLUDED
#define MONITOR_CONTROL_H_INCLUDED

/* control.c: the controller, its input and CAKE bandwidth changes. */

#include "controller/controller.h"
#include "latency/parser.h"
#include "latency/tracker.h"

#include <stdbool.h>
#include <stdint.h>

struct monitor;

struct monitor_control {
	struct controller controller;
	bool initial_shaper_reported;
};

int control_start(struct monitor *monitor);

void control_update_compensation(struct monitor *monitor);

bool control_low_load(const struct monitor *monitor);

void control_update(
	struct monitor *monitor,
	const struct latency_observation *latency,
	const struct latency_sample *sample
);

void control_enforce_minimum(struct monitor *monitor, uint64_t timestamp_us);

#endif
