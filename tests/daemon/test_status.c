/* status_snapshot() from synthetic daemon state. */
#include "daemon/status.c"

#include <assert.h>
#include <stdio.h>

static bool estimate_open;
static struct tcpdelay_estimate supplied_estimate;

bool tcp_estimate(struct daemon *daemon, uint64_t timestamp_us, struct tcpdelay_estimate *estimate)
{
	(void)daemon;
	(void)timestamp_us;
	*estimate = supplied_estimate;
	return estimate_open;
}

static void set_direction(
	struct links_direction *link,
	const char *interface,
	uint64_t shaper_bps,
	uint64_t traffic_bps
)
{
	struct cake_observation *cake = &link->cake;

	link->interface = interface;
	link->cake_state = LINKS_CAKE_AVAILABLE;
	cake->has_bandwidth = shaper_bps != 0U;
	cake->bandwidth_bps = shaper_bps;
	link->traffic_valid = traffic_bps != 0U;
	link->traffic_rate_bps = traffic_bps;
}

static void test_running_with_tcp_estimate(void)
{
	struct daemon daemon = { 0 };
	struct status_snapshot snapshot;

	set_direction(&daemon.links.directions[DIRECTION_DOWNLOAD], "ifb4wan", 85000000U, 61200000U);
	set_direction(&daemon.links.directions[DIRECTION_UPLOAD], "wan", 14000000U, 7000000U);
	daemon.shaper.controller.directions[DIRECTION_DOWNLOAD].average_delay_us = 3400;
	daemon.shaper.controller.directions[DIRECTION_UPLOAD].average_delay_us = -150;
	daemon.shaper.controller.directions[DIRECTION_UPLOAD].congestion =
		CONTROLLER_CONGESTION_DETECTED;
	daemon.activity.state = CONTROLLER_ACTIVITY_RUNNING;
	estimate_open = true;
	supplied_estimate = (struct tcpdelay_estimate){
		.valid[DIRECTION_DOWNLOAD] = true,
		.queue_us[DIRECTION_DOWNLOAD] = 2100,
		.valid[DIRECTION_UPLOAD] = false,
	};
	status_snapshot(&daemon, 1000U, &snapshot);
	assert(snapshot.state == CONTROLLER_ACTIVITY_RUNNING);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].available);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].shaper_bps == 85000000U);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].achieved_bps == 61200000U);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].load_e6 == 720000U);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].delay_us == 3400);
	assert(!snapshot.directions[DIRECTION_DOWNLOAD].bufferbloat);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].tcp_queue_valid &&
	       snapshot.directions[DIRECTION_DOWNLOAD].tcp_queue_us == 2100);
	assert(snapshot.directions[DIRECTION_UPLOAD].load_e6 == 500000U);
	assert(snapshot.directions[DIRECTION_UPLOAD].delay_us == -150);
	assert(snapshot.directions[DIRECTION_UPLOAD].bufferbloat);
	assert(!snapshot.directions[DIRECTION_UPLOAD].tcp_queue_valid);
}

static void test_missing_cake_and_no_capture(void)
{
	struct daemon daemon = { 0 };
	struct status_snapshot snapshot;

	set_direction(&daemon.links.directions[DIRECTION_DOWNLOAD], "ifb4wan", 85000000U, 1000U);
	daemon.links.directions[DIRECTION_UPLOAD].interface = "wan";
	daemon.links.directions[DIRECTION_UPLOAD].cake_state = LINKS_CAKE_NOT_FOUND;
	daemon.activity.state = CONTROLLER_ACTIVITY_IDLE;
	estimate_open = false;
	status_snapshot(&daemon, 1000U, &snapshot);
	assert(snapshot.state == CONTROLLER_ACTIVITY_IDLE);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].available);
	assert(!snapshot.directions[DIRECTION_DOWNLOAD].tcp_queue_valid);
	assert(!snapshot.directions[DIRECTION_UPLOAD].available);
	assert(snapshot.directions[DIRECTION_UPLOAD].shaper_bps == 0U &&
	       snapshot.directions[DIRECTION_UPLOAD].load_e6 == 0U);
	assert(snapshot.directions[DIRECTION_UPLOAD].interface != NULL);
}

static void test_unknown_shaper_or_traffic_gives_no_load(void)
{
	struct daemon daemon = { 0 };
	struct status_snapshot snapshot;

	set_direction(&daemon.links.directions[DIRECTION_DOWNLOAD], "ifb4wan", 85000000U, 0U);
	set_direction(&daemon.links.directions[DIRECTION_UPLOAD], "wan", 0U, 5000000U);
	estimate_open = false;
	status_snapshot(&daemon, 1000U, &snapshot);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].available &&
	       snapshot.directions[DIRECTION_DOWNLOAD].load_e6 == 0U);
	assert(snapshot.directions[DIRECTION_DOWNLOAD].achieved_bps == 0U);
	assert(!snapshot.directions[DIRECTION_UPLOAD].available);
	assert(snapshot.directions[DIRECTION_UPLOAD].load_e6 == 0U);
}

int main(void)
{
	test_running_with_tcp_estimate();
	test_missing_cake_and_no_capture();
	test_unknown_shaper_or_traffic_gives_no_load();
	(void)puts("daemon status tests passed");
	return 0;
}
