#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/helpers.h"

/* A direction with no CAKE reports only its interface. */
static void status_direction(
	const struct monitor_direction *link,
	const struct controller_direction *control,
	struct status_direction *status
)
{
	const struct cake_observation *cake = &link->cake;

	*status = (struct status_direction){ .interface = link->interface };
	if (link->cake_state != CAKE_OBSERVATION_AVAILABLE || !cake->has_bandwidth)
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

void status_snapshot(
	struct monitor *monitor,
	uint64_t timestamp_us,
	struct status_snapshot *snapshot
)
{
	const struct monitor_links *links = &monitor->links;
	const struct monitor_control *control = &monitor->control;
	const struct controller *controller = &control->controller;
	const struct controller_activity *activity = &monitor->activity;
	struct status_direction *download = &snapshot->download;
	struct status_direction *upload = &snapshot->upload;
	struct tcpdelay_estimate estimate;

	status_direction(&links->download, &controller->download, download);
	status_direction(&links->upload, &controller->upload, upload);
	snapshot->state = activity->state;
	if (!tcp_estimate(monitor, timestamp_us, &estimate))
		return;
	download->tcp_queue_valid = download->available && estimate.download_valid;
	download->tcp_queue_us = estimate.download_queue_us;
	upload->tcp_queue_valid = upload->available && estimate.upload_valid;
	upload->tcp_queue_us = estimate.upload_queue_us;
}
