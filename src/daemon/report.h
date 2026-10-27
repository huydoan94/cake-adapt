#ifndef DAEMON_REPORT_H_INCLUDED
#define DAEMON_REPORT_H_INCLUDED

/*
 * report.c: what the shaper and the activity state report: the messages for a
 * changed decision, the DATA and SUMMARY records, and the names they print
 * (the activity names inline, for the status object too).
 */

#include "common/direction.h"
#include "controller/activity.h"
#include "controller/controller.h"
#include "constants/states.h"

struct daemon;
struct shaper_step;

const char *report_rate_reason_name(enum controller_rate_reason reason);

/* An activity state as the status object names it ("running"). */
static inline const char *report_activity_name(enum controller_activity_state state)
{
	static const char *const names[] = {
		[CONTROLLER_ACTIVITY_RUNNING] = STATE_RUNNING,
		[CONTROLLER_ACTIVITY_IDLE] = STATE_IDLE,
		[CONTROLLER_ACTIVITY_STALL] = STATE_STALL,
	};

	return names[state];
}

/* An activity state as log messages name it ("RUNNING"). */
static inline const char *report_activity_log_name(enum controller_activity_state state)
{
	static const char *const names[] = {
		[CONTROLLER_ACTIVITY_RUNNING] = STATE_RUNNING_UPPER,
		[CONTROLLER_ACTIVITY_IDLE] = STATE_IDLE_UPPER,
		[CONTROLLER_ACTIVITY_STALL] = STATE_STALL_UPPER,
	};

	return names[state];
}

/* The messages for what changed in one direction's decision, in their fixed order. */
void report_decision(
	const struct daemon *daemon,
	const struct shaper_step *step,
	enum direction direction
);

/* The step's DATA and SUMMARY records, each when the configuration asks for it. */
void report_records(const struct daemon *daemon, const struct shaper_step *step);

#endif
