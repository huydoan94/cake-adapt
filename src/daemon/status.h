#ifndef DAEMON_STATUS_H_INCLUDED
#define DAEMON_STATUS_H_INCLUDED

/* status.c: the instance's status snapshot, from state the daemon already holds. */

#include "common/direction.h"
#include "controller/activity.h"
#include "controller/controller.h"

#include <stdbool.h>
#include <stdint.h>

struct daemon;

struct status_direction {
	const char *interface;
	bool available;
	uint64_t shaper_bps;
	uint64_t achieved_bps;
	uint64_t load_e6;
	int64_t delay_us;
	bool bufferbloat;
	bool tcp_queue_valid;
	int64_t tcp_queue_us;
};

struct status_snapshot {
	enum controller_activity_state state;
	struct status_direction directions[DIRECTION_COUNT];
};

/* One instance's current status, from state the daemon already holds. */
void status_snapshot(struct daemon *daemon, uint64_t timestamp_us, struct status_snapshot *snapshot);

#endif
