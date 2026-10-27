#define _GNU_SOURCE

#include "daemon/state.h"

#include "common/error.h"
#include "common/arith.h"
#include "common/clock.h"
#include "common/convert.h"
#include "common/rate.h"
#include "logging/log.h"
#include "logging/records.h"
#include "constants/cake.h"
#include "constants/states.h"
#include "constants/text.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <linux/pkt_sched.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void observe_traffic(struct links_direction *direction, uint64_t timestamp_us)
{
	struct cake_observation *cake = &direction->cake;
	struct traffic_sample sample;
	enum traffic_update_result update_result;

	direction->traffic_rate_bps = 0U;
	direction->traffic_valid = false;
	if (!direction->cake_valid || !cake->has_basic_stats) {
		if (direction->traffic_state != LINKS_TRAFFIC_UNAVAILABLE)
			log_message(
				LOG_LEVEL_WARNING,
				"traffic observation unavailable: direction=%s interface=%s",
				direction->name,
				direction->interface
			);
		direction->traffic_state = LINKS_TRAFFIC_UNAVAILABLE;
		traffic_init(&direction->traffic_monitor);
		return;
	}

	if (direction->traffic_state == LINKS_TRAFFIC_UNAVAILABLE)
		log_message(
			LOG_LEVEL_NOTICE,
			"traffic observation recovered: direction=%s interface=%s",
			direction->name,
			direction->interface
		);
	direction->traffic_state = LINKS_TRAFFIC_AVAILABLE;

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

static void
log_cake_discovery(const char *interface, const struct cake_observation *observation, bool recovered)
{
	const struct netlink_qdisc_id *qdisc = &observation->qdisc;
	char bandwidth[CAKE_BANDWIDTH_TEXT_SIZE];

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
			qdisc->handle,
			bandwidth
		);
	} else {
		log_message(
			LOG_LEVEL_INFO,
			"CAKE discovered: interface=%s qdisc=%s handle=0x%08" PRIx32
			" bandwidth=%s",
			interface,
			observation->kind,
			qdisc->handle,
			bandwidth
		);
	}
}

static void record_cake_read(
	struct links_direction *direction,
	const struct cake_read *read,
	uint64_t timestamp_us,
	uint64_t retry_interval_us
)
{
	switch (read->result) {
	case CAKE_READ_FOUND:
		if (direction->cake_state != LINKS_CAKE_AVAILABLE) {
			log_cake_discovery(
				direction->interface,
				&direction->cake,
				direction->cake_state != LINKS_CAKE_UNKNOWN
			);
		}
		direction->cake_state = LINKS_CAKE_AVAILABLE;
		direction->cake_valid = true;
		direction->next_cake_observation_us = 0U;
		return;
	case CAKE_READ_NOT_FOUND:
		if (direction->cake_state != LINKS_CAKE_NOT_FOUND) {
			log_message(
				LOG_LEVEL_WARNING,
				direction->cake_state == LINKS_CAKE_AVAILABLE ?
					"CAKE observation degraded: no CAKE qdisc found on interface=%s" :
					"CAKE not found: interface=%s; observation will retry",
				direction->interface
			);
		}
		direction->cake_state = LINKS_CAKE_NOT_FOUND;
		/* RTM_NEWQDISC wakes discovery when CAKE is created. */
		direction->next_cake_observation_us = UINT64_MAX;
		return;
	case CAKE_READ_ERROR:
		if (direction->cake_state != LINKS_CAKE_FAILED) {
			log_message(
				LOG_LEVEL_WARNING,
				"CAKE observation degraded: interface=%s: %s",
				direction->interface,
				read->error
			);
		}
		direction->cake_state = LINKS_CAKE_FAILED;
		break;
	}
	direction->next_cake_observation_us = timestamp_us + retry_interval_us;
}

/* Directions whose retry time has come share one qdisc dump. */
static void observe_cake(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_links *links = &daemon->links;
	uint64_t retry_interval_us = daemon->config->interface_up_check_interval_us;
	struct links_direction *const directions[] = {
		&links->directions[DIRECTION_UPLOAD],
		&links->directions[DIRECTION_DOWNLOAD],
	};
	struct cake_read reads[ARRAY_SIZE(directions)];
	struct links_direction *due[ARRAY_SIZE(directions)];
	size_t count = 0U;
	size_t index;

	for (index = 0U; index < ARRAY_SIZE(directions); index++) {
		struct links_direction *direction = directions[index];

		direction->cake_valid = false;
		if (timestamp_us < direction->next_cake_observation_us)
			continue;
		reads[count] = (struct cake_read){ .interface = direction->interface,
						   .observation = &direction->cake };
		due[count++] = direction;
	}
	cake_read(&daemon->netlink, reads, count);
	for (index = 0U; index < count; index++)
		record_cake_read(due[index], &reads[index], timestamp_us, retry_interval_us);
}

void links_apply_cadence(struct daemon *daemon)
{
	struct daemon_links *links = &daemon->links;

	if (!links->cadence_initialized || links->cadence_applied)
		return;
	if (uloop_interval_set(&daemon->traffic_timer, us_to_ms(links->cadence_us)) != 0) {
		log_message(
			LOG_LEVEL_WARNING,
			"could not apply compensated traffic cadence: %s",
			strerror(errno)
		);
		return;
	}
	links->cadence_applied = true;
}

bool links_ready(const struct daemon *daemon)
{
	const struct daemon_links *links = &daemon->links;

	for (size_t index = 0U; index < DIRECTION_COUNT; index++) {
		const struct links_direction *direction = &links->directions[index];

		if (direction->cake_state != LINKS_CAKE_AVAILABLE)
			return false;
	}
	return true;
}

/* This tick read the direction's CAKE, with the MTU and bandwidth its wire cadence needs. */
static bool direction_wire_ready(const struct links_direction *direction)
{
	const struct cake_observation *cake = &direction->cake;

	return direction->cake_valid && cake->has_mtu && cake->has_bandwidth;
}

bool links_wire_ready(const struct daemon *daemon)
{
	const struct daemon_links *links = &daemon->links;

	if (!links_ready(daemon))
		return false;
	for (size_t index = 0U; index < DIRECTION_COUNT; index++)
		if (!direction_wire_ready(&links->directions[index]))
			return false;
	return true;
}

/* A LOAD record needs an achieved rate and the CAKE rate; valid traffic implies valid CAKE. */
static bool direction_has_load(const struct links_direction *direction)
{
	const struct cake_observation *cake = &direction->cake;

	return direction->traffic_valid && cake->has_bandwidth;
}

/* The LOAD record, when both directions have one. */
static void log_load_stats(const struct daemon_links *links)
{
	struct log_load_record record = { 0 };

	for (size_t index = 0U; index < DIRECTION_COUNT; index++) {
		const struct links_direction *direction = &links->directions[index];
		const struct cake_observation *cake = &direction->cake;

		if (!direction_has_load(direction))
			return;
		record.achieved_rate_bps[index] = direction->traffic_rate_bps;
		record.cake_rate_bps[index] = cake->bandwidth_bps;
	}
	log_load(&record);
}

void links_observe(struct daemon *daemon)
{
	const struct config *config = daemon->config;
	struct daemon_links *links = &daemon->links;
	uint64_t timestamp_us;
	size_t index;

	if (!read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		daemon_degraded(
			&links->clock_failed,
			"traffic observation degraded: monotonic clock failed: %s",
			strerror(errno)
		);
		for (index = 0U; index < DIRECTION_COUNT; index++) {
			struct links_direction *direction = &links->directions[index];

			direction->cake_valid = false;
			direction->traffic_valid = false;
			traffic_init(&direction->traffic_monitor);
		}
		return;
	}

	observe_cake(daemon, timestamp_us);
	daemon_recovered(
		&links->clock_failed,
		"traffic observation recovered: monotonic clock available"
	);
	shaper_update_compensation(daemon);
	if (!links->cadence_initialized && links_wire_ready(daemon)) {
		/* One wire packet's time in each direction at its base rate. */
		uint64_t serialization_sum_us = 0U;

		for (index = 0U; index < DIRECTION_COUNT; index++) {
			const struct links_direction *direction = &links->directions[index];
			const struct config_direction *limits = &config->directions[index];

			serialization_sum_us = saturating_add(
				serialization_sum_us,
				serialization_us(
					cake_max_wire_packet_bits(&direction->cake),
					limits->base_rate_bps
				)
			);
		}
		links->cadence_us = traffic_compensated_interval_us(
			config->monitor_achieved_rates_interval_us,
			serialization_sum_us
		);
		links->cadence_initialized = true;
	}
	for (index = 0U; index < DIRECTION_COUNT; index++)
		observe_traffic(&links->directions[index], timestamp_us);

	if (config->output_load_stats)
		log_load_stats(links);
}

static struct links_direction *
event_direction(struct daemon *daemon, const struct netlink_qdisc_event *event)
{
	const struct netlink_qdisc_id *qdisc = &event->qdisc;
	struct daemon_links *links = &daemon->links;
	struct links_direction *directions = links->directions;
	size_t index;

	if (qdisc->parent != TC_H_ROOT)
		return NULL;
	for (index = 0U; index < DIRECTION_COUNT; index++) {
		const struct cake_observation *cake = &directions[index].cake;
		const struct netlink_qdisc_id *watched = &cake->qdisc;

		if (watched->interface_index == qdisc->interface_index)
			return &directions[index];
	}
	/* An unknown index may belong to a monitored interface that was recreated. */
	for (index = 0U; index < DIRECTION_COUNT; index++) {
		struct links_direction *direction = &directions[index];
		struct cake_observation *cake = &direction->cake;
		struct netlink_qdisc_id *watched = &cake->qdisc;

		if (if_nametoindex(direction->interface) == qdisc->interface_index) {
			watched->interface_index = qdisc->interface_index;
			cake->has_mtu = false;
			return direction;
		}
	}
	return NULL;
}

/* The event concerns the CAKE this direction observes now. */
static bool watching_qdisc(const struct links_direction *direction, uint32_t handle)
{
	const struct netlink_qdisc_id *watched = &direction->cake.qdisc;

	return direction->cake_state == LINKS_CAKE_AVAILABLE && watched->handle == handle;
}

static void reset_traffic_observation(struct links_direction *direction)
{
	/* Preserve the sample ID: the controller retains its last consumed ID. */
	direction->traffic_valid = false;
	direction->traffic_state = LINKS_TRAFFIC_UNKNOWN;
	direction->traffic_rate_bps = 0U;
	traffic_init(&direction->traffic_monitor);
}

static void process_qdisc_event(struct netlink *netlink, const struct netlink_qdisc_event *event)
{
	const struct netlink_qdisc_id *qdisc = &event->qdisc;
	struct daemon *daemon = __extension__ container_of(netlink, struct daemon, netlink);
	struct daemon_links *links = &daemon->links;
	struct links_direction *direction = event_direction(daemon, event);

	if (direction == NULL)
		return;
	if (event->type == NETLINK_QDISC_EVENT_REMOVED) {
		if (!watching_qdisc(direction, qdisc->handle))
			return;
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
		direction->cake_state = LINKS_CAKE_NOT_FOUND;
		direction->next_cake_observation_us = UINT64_MAX;
		reset_traffic_observation(direction);
		if (direction == &links->directions[DIRECTION_UPLOAD])
			tcp_close(daemon);
		pingers_close(daemon);
		return;
	}

	/* Bandwidth changes notify RTM_NEWQDISC with the existing handle. */
	if (watching_qdisc(direction, qdisc->handle))
		return;
	if (direction->cake_state == LINKS_CAKE_AVAILABLE)
		direction->cake_state = LINKS_CAKE_NOT_FOUND;
	direction->cake_valid = false;
	direction->next_cake_observation_us = 0U;
	reset_traffic_observation(direction);
	links->qdisc_refresh = true;
}

static void handle_qdisc_events(struct uloop_fd *descriptor, unsigned int events)
{
	struct daemon *daemon =
		__extension__ container_of(descriptor, struct daemon, links.qdisc_events);
	struct daemon_links *links = &daemon->links;
	char error[ERROR_SIZE] = { 0 };

	(void)events;
	if (netlink_receive_qdisc_events(&daemon->netlink, error, sizeof(error)) != 0) {
		log_message(LOG_LEVEL_ERROR, "qdisc lifecycle monitoring failed: %s", error);
		daemon->result = -1;
		uloop_end();
		return;
	}
	if (links->qdisc_refresh) {
		links->qdisc_refresh = false;
		daemon_tick(daemon);
	}
}

int links_watch_events(struct daemon *daemon)
{
	struct uloop_fd *events = &daemon->links.qdisc_events;
	struct netlink *netlink = &daemon->netlink;
	char error[ERROR_SIZE] = { 0 };

	events->cb = handle_qdisc_events;
	netlink->netlink_qdisc_event = process_qdisc_event;
	if (netlink_subscribe_qdiscs(netlink, error, sizeof(error)) != 0) {
		log_message(LOG_LEVEL_ERROR, "%s", error);
		return -1;
	}
	events->fd = netlink_event_descriptor(netlink);
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
