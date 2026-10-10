#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <linux/pkt_sched.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void observe_traffic(struct monitor_direction *direction, uint64_t timestamp_us)
{
	struct cake_observation *cake = &direction->cake;
	struct traffic_sample sample;
	enum traffic_update_result update_result;

	direction->traffic_rate_bps = 0U;
	direction->traffic_valid = false;
	if (!direction->cake_valid || !cake->has_basic_stats) {
		if (direction->traffic_state != TRAFFIC_OBSERVATION_UNAVAILABLE)
			log_message(
				LOG_LEVEL_WARNING,
				"traffic observation unavailable: direction=%s interface=%s",
				direction->name,
				direction->interface
			);
		direction->traffic_state = TRAFFIC_OBSERVATION_UNAVAILABLE;
		traffic_init(&direction->traffic_monitor);
		return;
	}

	if (direction->traffic_state == TRAFFIC_OBSERVATION_UNAVAILABLE)
		log_message(
			LOG_LEVEL_NOTICE,
			"traffic observation recovered: direction=%s interface=%s",
			direction->name,
			direction->interface
		);
	direction->traffic_state = TRAFFIC_OBSERVATION_AVAILABLE;

	sample = (struct traffic_sample){ .bytes = cake->bytes,
					  .qdisc = cake->qdisc,
					  .timestamp_us = timestamp_us };
	update_result =
		traffic_update(&direction->traffic_monitor, &sample, &direction->traffic_rate_bps);

	switch (update_result) {
	case TRAFFIC_UPDATE_BASELINE:
		log_message(
			LOG_LEVEL_INFO,
			"traffic observation initialized: direction=%s interface=%s",
			direction->name,
			direction->interface
		);
		break;
	case TRAFFIC_UPDATE_RATES:
		direction->traffic_sample_id++;
		direction->traffic_valid = true;
		return;
	case TRAFFIC_UPDATE_COUNTER_RESET:
		log_message(
			LOG_LEVEL_WARNING,
			"CAKE traffic counter reset; re-baselining: direction=%s interface=%s",
			direction->name,
			direction->interface
		);
		break;
	case TRAFFIC_UPDATE_QDISC_REPLACED:
		log_message(
			LOG_LEVEL_NOTICE,
			"CAKE qdisc changed; traffic observation re-baselined:"
			" direction=%s interface=%s handle=0x%08" PRIx32,
			direction->name,
			direction->interface,
			sample.qdisc.handle
		);
		break;
	case TRAFFIC_UPDATE_INVALID_INTERVAL:
		log_message(
			LOG_LEVEL_WARNING,
			"traffic sample interval was invalid: direction=%s interface=%s",
			direction->name,
			direction->interface
		);
		break;
	}
}

/* Longest "<rate> bit/s": UINT64_MAX has 20 digits. */
#define BANDWIDTH_TEXT_SIZE 32U

static void
log_cake_discovery(const char *interface, const struct cake_observation *observation, bool recovered)
{
	char bandwidth[BANDWIDTH_TEXT_SIZE];

	if (!observation->has_bandwidth || observation->bandwidth_bps == 0U) {
		(void)snprintf(bandwidth, sizeof(bandwidth), "%s", STATE_UNLIMITED);
	} else {
		(void)snprintf(
			bandwidth,
			sizeof(bandwidth),
			"%" PRIu64 " bit/s",
			observation->bandwidth_bps
		);
	}
	if (recovered) {
		log_message(
			LOG_LEVEL_NOTICE,
			"CAKE observation recovered: interface=%s qdisc=%s handle=0x%08" PRIx32
			" bandwidth=%s",
			interface,
			observation->kind,
			observation->qdisc.handle,
			bandwidth
		);
	} else {
		log_message(
			LOG_LEVEL_INFO,
			"CAKE discovered: interface=%s qdisc=%s handle=0x%08" PRIx32
			" bandwidth=%s",
			interface,
			observation->kind,
			observation->qdisc.handle,
			bandwidth
		);
	}
}

static void record_cake_read(
	struct monitor_direction *direction,
	const struct cake_read *read,
	uint64_t timestamp_us,
	uint64_t retry_interval_us
)
{
	switch (read->result) {
	case CAKE_READ_FOUND:
		if (direction->cake_state != CAKE_OBSERVATION_AVAILABLE) {
			log_cake_discovery(
				direction->interface,
				&direction->cake,
				direction->cake_state != CAKE_OBSERVATION_UNKNOWN
			);
		}
		direction->cake_state = CAKE_OBSERVATION_AVAILABLE;
		direction->cake_valid = true;
		direction->next_cake_observation_us = 0U;
		return;
	case CAKE_READ_NOT_FOUND:
		if (direction->cake_state != CAKE_OBSERVATION_NOT_FOUND) {
			log_message(
				LOG_LEVEL_WARNING,
				direction->cake_state == CAKE_OBSERVATION_AVAILABLE ?
					"CAKE observation degraded: no CAKE qdisc found on interface=%s" :
					"CAKE not found: interface=%s; observation will retry",
				direction->interface
			);
		}
		direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
		/* RTM_NEWQDISC wakes discovery when CAKE is created. */
		direction->next_cake_observation_us = UINT64_MAX;
		return;
	case CAKE_READ_ERROR:
		if (direction->cake_state != CAKE_OBSERVATION_FAILED) {
			log_message(
				LOG_LEVEL_WARNING,
				"CAKE observation degraded: interface=%s: %s",
				direction->interface,
				read->error
			);
		}
		direction->cake_state = CAKE_OBSERVATION_FAILED;
		break;
	}
	direction->next_cake_observation_us = timestamp_us + retry_interval_us;
}

/* Directions whose retry time has come share one qdisc dump. */
static void observe_cake(struct monitor *monitor, uint64_t timestamp_us)
{
	struct monitor_links *links = &monitor->links;
	uint64_t retry_interval_us = monitor->config->interface_up_check_interval_us;
	struct monitor_direction *const directions[] = {
		&links->upload,
		&links->download,
	};
	struct cake_read reads[ARRAY_SIZE(directions)];
	struct monitor_direction *due[ARRAY_SIZE(directions)];
	size_t count = 0U;
	size_t index;

	for (index = 0U; index < ARRAY_SIZE(directions); index++) {
		struct monitor_direction *direction = directions[index];

		direction->cake_valid = false;
		if (timestamp_us < direction->next_cake_observation_us)
			continue;
		reads[count] = (struct cake_read){ .interface = direction->interface,
						   .observation = &direction->cake };
		due[count++] = direction;
	}
	cake_read(&monitor->netlink, reads, count);
	for (index = 0U; index < count; index++)
		record_cake_read(due[index], &reads[index], timestamp_us, retry_interval_us);
}

void links_apply_cadence(struct monitor *monitor)
{
	struct monitor_links *links = &monitor->links;

	if (!links->cadence_initialized || links->cadence_applied)
		return;
	if (uloop_interval_set(&monitor->traffic_timer, us_to_ms(links->cadence_us)) != 0) {
		log_message(
			LOG_LEVEL_WARNING,
			"could not apply compensated traffic cadence: %s",
			strerror(errno)
		);
		return;
	}
	links->cadence_applied = true;
}

bool links_ready(const struct monitor *monitor)
{
	const struct monitor_links *links = &monitor->links;

	return links->download.cake_state == CAKE_OBSERVATION_AVAILABLE &&
	       links->upload.cake_state == CAKE_OBSERVATION_AVAILABLE;
}

bool links_wire_ready(const struct monitor *monitor)
{
	const struct monitor_links *links = &monitor->links;
	const struct monitor_direction *download = &links->download;
	const struct monitor_direction *upload = &links->upload;

	return links_ready(monitor) && download->cake_valid && upload->cake_valid &&
	       download->cake.has_mtu && upload->cake.has_mtu && download->cake.has_bandwidth &&
	       upload->cake.has_bandwidth;
}

static void
log_load_stats(const struct monitor_direction *download, const struct monitor_direction *upload)
{
	const struct cake_observation *download_cake = &download->cake;
	const struct cake_observation *upload_cake = &upload->cake;
	const struct log_load_record record = {
		.download_achieved_rate_bps = download->traffic_rate_bps,
		.upload_achieved_rate_bps = upload->traffic_rate_bps,
		.cake_download_rate_bps = download_cake->bandwidth_bps,
		.cake_upload_rate_bps = upload_cake->bandwidth_bps,
	};

	log_load(&record);
}

void links_observe(struct monitor *monitor)
{
	const struct config *config = monitor->config;
	struct monitor_links *links = &monitor->links;
	struct monitor_direction *download = &links->download;
	struct monitor_direction *upload = &links->upload;
	uint64_t timestamp_us;

	if (!read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		if (!links->clock_failed) {
			log_message(
				LOG_LEVEL_WARNING,
				"traffic observation degraded: monotonic clock failed: %s",
				strerror(errno)
			);
		}
		links->clock_failed = true;
		download->cake_valid = false;
		upload->cake_valid = false;
		download->traffic_valid = false;
		upload->traffic_valid = false;
		traffic_init(&download->traffic_monitor);
		traffic_init(&upload->traffic_monitor);
		return;
	}

	observe_cake(monitor, timestamp_us);
	if (links->clock_failed) {
		log_message(
			LOG_LEVEL_NOTICE,
			"traffic observation recovered: monotonic clock available"
		);
		links->clock_failed = false;
	}
	control_update_compensation(monitor);
	if (!links->cadence_initialized && links_wire_ready(monitor)) {
		links->cadence_us = traffic_compensated_interval_us(
			config->monitor_achieved_rates_interval_us,
			saturating_add(
				serialization_us(
					cake_max_wire_packet_bits(&download->cake),
					config->download.base_rate_bps
				),
				serialization_us(
					cake_max_wire_packet_bits(&upload->cake),
					config->upload.base_rate_bps
				)
			)
		);
		links->cadence_initialized = true;
	}
	observe_traffic(download, timestamp_us);
	observe_traffic(upload, timestamp_us);

	/* Valid traffic comes from a valid CAKE observation. */
	if (config->output_load_stats && download->traffic_valid && upload->traffic_valid &&
	    download->cake.has_bandwidth && upload->cake.has_bandwidth) {
		log_load_stats(download, upload);
	}
}

static struct monitor_direction *
event_direction(struct monitor *monitor, const struct qdisc_event *event)
{
	const struct qdisc_id *qdisc = &event->qdisc;
	struct monitor_links *links = &monitor->links;
	struct monitor_direction *directions[] = {
		&links->download,
		&links->upload,
	};
	size_t index;

	if (qdisc->parent != TC_H_ROOT)
		return NULL;
	for (index = 0U; index < ARRAY_SIZE(directions); index++)
		if (directions[index]->cake.qdisc.interface_index == qdisc->interface_index)
			return directions[index];
	/* An unknown index may belong to a monitored interface that was recreated. */
	for (index = 0U; index < ARRAY_SIZE(directions); index++) {
		struct cake_observation *cake = &directions[index]->cake;

		if (if_nametoindex(directions[index]->interface) == qdisc->interface_index) {
			cake->qdisc.interface_index = qdisc->interface_index;
			cake->has_mtu = false;
			return directions[index];
		}
	}
	return NULL;
}

static void reset_traffic_observation(struct monitor_direction *direction)
{
	/* Preserve the sample ID: the controller retains its last consumed ID. */
	direction->traffic_valid = false;
	direction->traffic_state = TRAFFIC_OBSERVATION_UNKNOWN;
	direction->traffic_rate_bps = 0U;
	traffic_init(&direction->traffic_monitor);
}

static void process_qdisc_event(struct netlink *netlink, const struct qdisc_event *event)
{
	const struct qdisc_id *qdisc = &event->qdisc;
	struct monitor *monitor = __extension__ container_of(netlink, struct monitor, netlink);
	struct monitor_links *links = &monitor->links;
	struct monitor_direction *direction = event_direction(monitor, event);

	if (direction == NULL)
		return;
	if (event->type == QDISC_REMOVED) {
		if (direction->cake_state != CAKE_OBSERVATION_AVAILABLE ||
		    direction->cake.qdisc.handle != qdisc->handle) {
			return;
		}
		log_message(
			LOG_LEVEL_NOTICE,
			"CAKE removed: direction=%s interface=%s handle=0x%08" PRIx32
			"; monitoring suspended",
			direction->name,
			direction->interface,
			qdisc->handle
		);
		memset(&direction->cake, 0, sizeof(direction->cake));
		direction->cake_valid = false;
		direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
		direction->next_cake_observation_us = UINT64_MAX;
		reset_traffic_observation(direction);
		if (direction == &links->upload)
			tcp_close(monitor);
		pingers_close(monitor);
		return;
	}

	/* Bandwidth changes notify RTM_NEWQDISC with the existing handle. */
	if (direction->cake_state == CAKE_OBSERVATION_AVAILABLE &&
	    direction->cake.qdisc.handle == qdisc->handle) {
		return;
	}
	if (direction->cake_state == CAKE_OBSERVATION_AVAILABLE)
		direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
	direction->cake_valid = false;
	direction->next_cake_observation_us = 0U;
	reset_traffic_observation(direction);
	links->qdisc_refresh = true;
}

static void handle_qdisc_events(struct uloop_fd *descriptor, unsigned int events)
{
	struct monitor *monitor =
		__extension__ container_of(descriptor, struct monitor, links.qdisc_events);
	struct monitor_links *links = &monitor->links;
	char error[ERROR_SIZE] = { 0 };

	(void)events;
	if (netlink_receive_qdisc_events(&monitor->netlink, error, sizeof(error)) != 0) {
		log_message(LOG_LEVEL_ERROR, "qdisc lifecycle monitoring failed: %s", error);
		monitor->result = -1;
		uloop_end();
		return;
	}
	if (links->qdisc_refresh) {
		links->qdisc_refresh = false;
		monitor_tick(monitor);
	}
}

int links_watch_events(struct monitor *monitor)
{
	struct uloop_fd *events = &monitor->links.qdisc_events;
	char error[ERROR_SIZE] = { 0 };

	events->cb = handle_qdisc_events;
	monitor->netlink.qdisc_event = process_qdisc_event;
	if (netlink_subscribe_qdiscs(&monitor->netlink, error, sizeof(error)) != 0) {
		log_message(LOG_LEVEL_ERROR, "%s", error);
		return -1;
	}
	events->fd = netlink_event_descriptor(&monitor->netlink);
	if (events->fd < 0 || uloop_fd_add(events, ULOOP_READ | ULOOP_ERROR_CB) != 0) {
		log_message(
			LOG_LEVEL_ERROR,
			"could not monitor qdisc lifecycle: %s",
			strerror(errno)
		);
		return -1;
	}
	return 0;
}
