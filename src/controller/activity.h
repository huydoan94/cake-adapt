#ifndef CONTROLLER_ACTIVITY_H_INCLUDED
#define CONTROLLER_ACTIVITY_H_INCLUDED

/*
 * Whether the link is in use: running, idle long enough to stop probing, or
 * stalled with no reflector answering.
 */

#include "controller/controller.h"

#include <stdbool.h>
#include <stdint.h>

enum controller_activity_state {
	CONTROLLER_RUNNING,
	CONTROLLER_IDLE,
	CONTROLLER_STALL
};

struct controller_activity_config {
	bool enable_sleep;
	uint64_t active_threshold_bps;
	uint64_t stall_threshold_bps;
	uint64_t sustained_idle_us;
	uint64_t stall_timeout_us;
	uint64_t global_timeout_us;
};

struct controller_activity_input {
	struct controller_direction_input download;
	struct controller_direction_input upload;
	uint64_t timestamp_us;
	uint64_t last_response_us;
	uint64_t last_pinger_start_us;
	uint64_t grace_until_us;
};

struct controller_activity {
	struct controller_activity_config config;
	enum controller_activity_state state;
	uint64_t idle_started_us;
	bool global_timeout_reported;
};

struct controller_activity_output {
	bool state_changed;
	bool check_stall_loads;
	bool global_timeout_started;
	bool restart_pingers;
};

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
