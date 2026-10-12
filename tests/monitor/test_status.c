/* status_snapshot() from synthetic monitor state. */
#include "monitor/status.c"

#include <assert.h>
#include <stdio.h>

static bool estimate_open;
static struct tcpdelay_estimate supplied_estimate;

bool tcp_estimate(struct monitor *monitor, uint64_t timestamp_us, struct tcpdelay_estimate *estimate)
{
	(void)monitor;
	(void)timestamp_us;
	*estimate = supplied_estimate;
	return estimate_open;
}

static void set_direction(
	struct monitor_direction *link,
	const char *interface,
	uint64_t shaper_bps,
	uint64_t traffic_bps
)
{
	struct cake_observation *cake = &link->cake;

	link->interface = interface;
	link->cake_state = CAKE_OBSERVATION_AVAILABLE;
	cake->has_bandwidth = shaper_bps != 0U;
	cake->bandwidth_bps = shaper_bps;
	link->traffic_valid = traffic_bps != 0U;
	link->traffic_rate_bps = traffic_bps;
}

static void test_running_with_tcp_estimate(void)
{
	struct monitor monitor = { 0 };
	struct status_snapshot snapshot;

	set_direction(&monitor.links.download, "ifb4wan", 85000000U, 61200000U);
	set_direction(&monitor.links.upload, "wan", 14000000U, 7000000U);
	monitor.control.controller.download.average_delay_us = 3400;
	monitor.control.controller.upload.average_delay_us = -150;
	monitor.control.controller.upload.congestion = CONTROLLER_CONGESTION_DETECTED;
	monitor.activity.state = CONTROLLER_RUNNING;
	estimate_open = true;
	supplied_estimate = (struct tcpdelay_estimate){
		.download_valid = true,
		.download_queue_us = 2100,
		.upload_valid = false,
	};
	status_snapshot(&monitor, 1000U, &snapshot);
	assert(snapshot.state == CONTROLLER_RUNNING);
	assert(snapshot.download.available);
	assert(snapshot.download.shaper_bps == 85000000U);
	assert(snapshot.download.achieved_bps == 61200000U);
	assert(snapshot.download.load_e6 == 720000U);
	assert(snapshot.download.delay_us == 3400);
	assert(!snapshot.download.bufferbloat);
	assert(snapshot.download.tcp_queue_valid && snapshot.download.tcp_queue_us == 2100);
	assert(snapshot.upload.load_e6 == 500000U);
	assert(snapshot.upload.delay_us == -150);
	assert(snapshot.upload.bufferbloat);
	assert(!snapshot.upload.tcp_queue_valid);
}

static void test_missing_cake_and_no_capture(void)
{
	struct monitor monitor = { 0 };
	struct status_snapshot snapshot;

	set_direction(&monitor.links.download, "ifb4wan", 85000000U, 1000U);
	monitor.links.upload.interface = "wan";
	monitor.links.upload.cake_state = CAKE_OBSERVATION_NOT_FOUND;
	monitor.activity.state = CONTROLLER_IDLE;
	estimate_open = false;
	status_snapshot(&monitor, 1000U, &snapshot);
	assert(snapshot.state == CONTROLLER_IDLE);
	assert(snapshot.download.available);
	assert(!snapshot.download.tcp_queue_valid);
	assert(!snapshot.upload.available);
	assert(snapshot.upload.shaper_bps == 0U && snapshot.upload.load_e6 == 0U);
	assert(snapshot.upload.interface != NULL);
}

static void test_unknown_shaper_or_traffic_gives_no_load(void)
{
	struct monitor monitor = { 0 };
	struct status_snapshot snapshot;

	set_direction(&monitor.links.download, "ifb4wan", 85000000U, 0U);
	set_direction(&monitor.links.upload, "wan", 0U, 5000000U);
	estimate_open = false;
	status_snapshot(&monitor, 1000U, &snapshot);
	assert(snapshot.download.available && snapshot.download.load_e6 == 0U);
	assert(snapshot.download.achieved_bps == 0U);
	assert(!snapshot.upload.available);
	assert(snapshot.upload.load_e6 == 0U);
}

int main(void)
{
	test_running_with_tcp_estimate();
	test_missing_cake_and_no_capture();
	test_unknown_shaper_or_traffic_gives_no_load();
	(void)puts("monitor status tests passed");
	return 0;
}
