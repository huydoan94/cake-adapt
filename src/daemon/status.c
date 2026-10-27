#define _GNU_SOURCE

#include "daemon/state.h"

#include "common/rate.h"

/* A direction with no CAKE reports only its interface. */
static void status_direction(
	const struct links_direction *link,
	const struct controller_direction *control,
	struct status_direction *status
)
{
	const struct cake_observation *cake = &link->cake;

	*status = (struct status_direction){ .interface = link->interface };
	if (link->cake_state != LINKS_CAKE_AVAILABLE || !cake->has_bandwidth)
		return;
	status->available = true;
	status->shaper_bps = cake->bandwidth_bps;
	if (link->traffic_valid) {
		status->achieved_bps = link->traffic_rate_bps;
		status->load_e6 = load_ratio_e6(link->traffic_rate_bps, cake->bandwidth_bps);
	}
	status->delay_us = control->average_delay_us;
	status->bufferbloat = control->congestion == CONTROLLER_CONGESTION_DETECTED;
}

void status_snapshot(struct daemon *daemon, uint64_t timestamp_us, struct status_snapshot *snapshot)
{
	const struct daemon_links *links = &daemon->links;
	const struct daemon_shaper *shaper = &daemon->shaper;
	const struct controller *controller = &shaper->controller;
	const struct controller_activity *activity = &daemon->activity;
	struct tcpdelay_estimate estimate;
	bool estimated;
	size_t index;

	for (index = 0U; index < DIRECTION_COUNT; index++)
		status_direction(
			&links->directions[index],
			&controller->directions[index],
			&snapshot->directions[index]
		);
	snapshot->state = activity->state;
	estimated = tcp_estimate(daemon, timestamp_us, &estimate);
	for (index = 0U; estimated && index < DIRECTION_COUNT; index++) {
		struct status_direction *direction = &snapshot->directions[index];

		direction->tcp_queue_valid = direction->available && estimate.valid[index];
		direction->tcp_queue_us = estimate.queue_us[index];
	}
}
