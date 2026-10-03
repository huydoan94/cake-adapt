#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/error.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <linux/pkt_sched.h>
#include <net/if.h>
#include <string.h>
#include <time.h>

static void observe_traffic(struct monitor_direction *direction, const struct timespec *timestamp)
{
	struct traffic_sample sample;
	enum traffic_update_result update_result;

	direction->traffic_rate_bits_per_second = 0U;
	direction->traffic_valid = false;
	if (!direction->cake_valid || !direction->cake.has_basic_stats) {
		if (direction->traffic_state != TRAFFIC_OBSERVATION_UNAVAILABLE) {
			log_message(LOG_LEVEL_WARNING,
				    "traffic observation unavailable: direction=%s interface=%s"
				    " source=CAKE basic stats",
				    direction->name, direction->interface);
		}
		direction->traffic_state = TRAFFIC_OBSERVATION_UNAVAILABLE;
		traffic_init(&direction->traffic_monitor);
		return;
	}

	if (direction->traffic_state == TRAFFIC_OBSERVATION_UNAVAILABLE) {
		log_message(LOG_LEVEL_NOTICE,
			    "traffic observation recovered: direction=%s interface=%s"
			    " source=CAKE basic stats",
			    direction->name, direction->interface);
	}
	direction->traffic_state = TRAFFIC_OBSERVATION_AVAILABLE;

	sample = (struct traffic_sample){ .bytes = direction->cake.bytes,
					  .qdisc_handle = direction->cake.handle,
					  .qdisc_parent = direction->cake.parent,
					  .timestamp = *timestamp };
	update_result = traffic_update(&direction->traffic_monitor, &sample,
				       &direction->traffic_rate_bits_per_second);

	switch (update_result) {
	case TRAFFIC_UPDATE_BASELINE:
		log_message(LOG_LEVEL_INFO,
			    "traffic observation initialized: direction=%s interface=%s"
			    " source=CAKE basic stats",
			    direction->name, direction->interface);
		break;
	case TRAFFIC_UPDATE_RATES:
		direction->traffic_sample_id++;
		direction->traffic_valid = true;
		return;
	case TRAFFIC_UPDATE_COUNTER_RESET:
		log_message(LOG_LEVEL_WARNING,
			    "CAKE traffic counter reset; re-baselining:"
			    " direction=%s interface=%s",
			    direction->name, direction->interface);
		break;
	case TRAFFIC_UPDATE_QDISC_REPLACED:
		log_message(LOG_LEVEL_NOTICE,
			    "CAKE qdisc changed; traffic observation re-baselined:"
			    " direction=%s interface=%s handle=0x%08" PRIx32,
			    direction->name, direction->interface, sample.qdisc_handle);
		break;
	case TRAFFIC_UPDATE_INVALID_INTERVAL:
		log_message(LOG_LEVEL_WARNING,
			    "traffic sample interval was invalid: direction=%s interface=%s",
			    direction->name, direction->interface);
		break;
	}
}

static void log_cake_discovery(const char *interface, const struct cake_observation *observation,
			       bool recovered)
{
	enum log_level level = recovered ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO;

	if (!observation->has_bandwidth || observation->bandwidth_bits_per_second == 0U) {
		log_message(level,
			    recovered
				    ? "CAKE observation recovered: interface=%s handle=0x%08" PRIx32
				      " bandwidth=unlimited"
				    : "CAKE discovered: interface=%s handle=0x%08" PRIx32
				      " bandwidth=unlimited",
			    interface, observation->handle);
		return;
	}

	log_message(level,
		    recovered ? "CAKE observation recovered: interface=%s handle=0x%08" PRIx32
				" bandwidth=%" PRIu64 " bit/s"
			      : "CAKE discovered: interface=%s handle=0x%08" PRIx32
				" bandwidth=%" PRIu64 " bit/s",
		    interface, observation->handle, observation->bandwidth_bits_per_second);
}

static void record_cake_read(struct monitor_direction *direction, const struct cake_read *read,
			     uint64_t timestamp_microseconds, uint64_t retry_interval_microseconds)
{
	switch (read->result) {
	case CAKE_READ_FOUND:
		if (direction->cake_state != CAKE_OBSERVATION_AVAILABLE) {
			log_cake_discovery(direction->interface, &direction->cake,
					   direction->cake_state != CAKE_OBSERVATION_UNKNOWN);
		}
		direction->cake_state = CAKE_OBSERVATION_AVAILABLE;
		direction->cake_valid = true;
		direction->next_cake_observation_microseconds = 0U;
		return;
	case CAKE_READ_NOT_FOUND:
		if (direction->cake_state != CAKE_OBSERVATION_NOT_FOUND) {
			log_message(
				LOG_LEVEL_WARNING,
				direction->cake_state == CAKE_OBSERVATION_AVAILABLE
					? "CAKE observation degraded: no CAKE qdisc found on interface=%s"
					: "CAKE not found: interface=%s; observation will retry",
				direction->interface);
		}
		direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
		/* RTM_NEWQDISC wakes discovery when CAKE is created. */
		direction->next_cake_observation_microseconds = UINT64_MAX;
		return;
	case CAKE_READ_ERROR:
		if (direction->cake_state != CAKE_OBSERVATION_FAILED) {
			log_message(LOG_LEVEL_WARNING,
				    "CAKE observation degraded: interface=%s: %s",
				    direction->interface, read->error);
		}
		direction->cake_state = CAKE_OBSERVATION_FAILED;
		break;
	}
	direction->next_cake_observation_microseconds =
		timestamp_microseconds + retry_interval_microseconds;
}

/* Directions whose retry time has come share one qdisc dump. */
static void observe_cake(struct netlink *netlink, struct monitor_direction *const directions[2],
			 uint64_t timestamp_microseconds, uint64_t retry_interval_microseconds)
{
	struct cake_read reads[2];
	struct monitor_direction *due[2];
	size_t count = 0U;
	size_t index;

	for (index = 0U; index < 2U; index++) {
		struct monitor_direction *direction = directions[index];

		direction->cake_valid = false;
		if (timestamp_microseconds < direction->next_cake_observation_microseconds)
			continue;
		reads[count] = (struct cake_read){ .interface = direction->interface,
						   .observation = &direction->cake };
		due[count++] = direction;
	}
	cake_read_all(netlink, reads, count);
	for (index = 0U; index < count; index++) {
		record_cake_read(due[index], &reads[index], timestamp_microseconds,
				 retry_interval_microseconds);
	}
}

void links_apply_cadence(struct monitor *monitor)
{
	struct monitor_links *links = &monitor->links;

	if (!links->cadence_initialized || links->cadence_applied)
		return;
	if (uloop_interval_set(&monitor->traffic_timer,
			       traffic_interval_milliseconds(links->cadence_microseconds)) != 0) {
		log_message(LOG_LEVEL_WARNING, "could not apply compensated traffic cadence: %s",
			    strerror(errno));
		return;
	}
	links->cadence_applied = true;
}

bool links_ready(const struct monitor *monitor)
{
	return monitor->links.download.cake_state == CAKE_OBSERVATION_AVAILABLE &&
	       monitor->links.upload.cake_state == CAKE_OBSERVATION_AVAILABLE;
}

bool links_wire_ready(const struct monitor *monitor)
{
	const struct monitor_direction *download = &monitor->links.download;
	const struct monitor_direction *upload = &monitor->links.upload;

	return links_ready(monitor) && download->cake_valid && upload->cake_valid &&
	       download->cake.has_mtu && upload->cake.has_mtu && download->cake.has_bandwidth &&
	       upload->cake.has_bandwidth;
}

static void log_load_stats(const struct monitor_direction *download,
			   const struct monitor_direction *upload)
{
	const struct log_load_record record = {
		.download_achieved_rate_kbps = download->traffic_rate_bits_per_second / KILOBIT,
		.upload_achieved_rate_kbps = upload->traffic_rate_bits_per_second / KILOBIT,
		.cake_download_rate_kbps = download->cake.bandwidth_bits_per_second / KILOBIT,
		.cake_upload_rate_kbps = upload->cake.bandwidth_bits_per_second / KILOBIT
	};

	log_load(&record);
}

void links_observe(struct monitor *monitor)
{
	const struct config *config = monitor->config;
	struct monitor_links *links = &monitor->links;
	struct monitor_direction *const directions[] = { &links->upload, &links->download };
	struct timespec traffic_timestamp;
	uint64_t timestamp_microseconds;

	if (clock_gettime(CLOCK_MONOTONIC, &traffic_timestamp) != 0) {
		if (!links->clock_failed) {
			log_message(LOG_LEVEL_WARNING,
				    "traffic observation degraded: monotonic clock failed: %s",
				    strerror(errno));
		}
		links->clock_failed = true;
		links->download.cake_valid = false;
		links->upload.cake_valid = false;
		links->download.traffic_valid = false;
		links->upload.traffic_valid = false;
		traffic_init(&links->download.traffic_monitor);
		traffic_init(&links->upload.traffic_monitor);
		return;
	}

	timestamp_microseconds = (uint64_t)traffic_timestamp.tv_sec * MICROSECONDS_PER_SECOND +
				 (uint64_t)traffic_timestamp.tv_nsec / NANOSECONDS_PER_MICROSECOND;
	observe_cake(&monitor->netlink, directions, timestamp_microseconds,
		     config->interface_up_check_interval_microseconds);
	if (links->clock_failed) {
		log_message(LOG_LEVEL_NOTICE,
			    "traffic observation recovered: monotonic clock available");
		links->clock_failed = false;
	}
	control_update_compensation(monitor);
	if (!links->cadence_initialized && links_wire_ready(monitor)) {
		links->cadence_microseconds = traffic_compensated_interval_microseconds(
			config->monitor_achieved_rates_interval_microseconds,
			cake_max_wire_packet_bits(&links->download.cake),
			config->base_download_rate_bits_per_second,
			cake_max_wire_packet_bits(&links->upload.cake),
			config->base_upload_rate_bits_per_second);
		links->cadence_initialized = true;
	}
	observe_traffic(&links->download, &traffic_timestamp);
	observe_traffic(&links->upload, &traffic_timestamp);

	if (config->output_load_stats && links->download.traffic_valid &&
	    links->upload.traffic_valid && links->download.cake_valid && links->upload.cake_valid &&
	    links->download.cake.has_bandwidth && links->upload.cake.has_bandwidth) {
		log_load_stats(&links->download, &links->upload);
	}
}

static struct monitor_direction *event_direction(struct monitor *monitor,
						 const struct qdisc_event *event)
{
	struct monitor_direction *directions[] = { &monitor->links.download,
						   &monitor->links.upload };
	size_t index;

	if (event->parent != TC_H_ROOT)
		return NULL;
	for (index = 0U; index < ARRAY_SIZE(directions); index++)
		if (directions[index]->cake.interface_index == event->interface_index)
			return directions[index];
	/* An unknown index may belong to a monitored interface that was recreated. */
	for (index = 0U; index < ARRAY_SIZE(directions); index++) {
		if (if_nametoindex(directions[index]->interface) == event->interface_index) {
			directions[index]->cake.interface_index = event->interface_index;
			directions[index]->cake.has_mtu = false;
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
	direction->traffic_rate_bits_per_second = 0U;
	traffic_init(&direction->traffic_monitor);
}

static void process_qdisc_event(const struct qdisc_event *event, void *context)
{
	struct monitor *monitor = context;
	struct monitor_direction *direction = event_direction(monitor, event);

	if (direction == NULL)
		return;
	if (event->type == QDISC_REMOVED) {
		if (direction->cake_state != CAKE_OBSERVATION_AVAILABLE ||
		    direction->cake.handle != event->handle) {
			return;
		}
		log_message(LOG_LEVEL_NOTICE,
			    "CAKE removed: direction=%s interface=%s handle=0x%08" PRIx32
			    "; monitoring suspended",
			    direction->name, direction->interface, event->handle);
		memset(&direction->cake, 0, sizeof(direction->cake));
		direction->cake_valid = false;
		direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
		direction->next_cake_observation_microseconds = UINT64_MAX;
		reset_traffic_observation(direction);
		pingers_close(monitor);
		return;
	}

	/* Bandwidth changes notify RTM_NEWQDISC with the existing handle. */
	if (direction->cake_state == CAKE_OBSERVATION_AVAILABLE &&
	    direction->cake.handle == event->handle) {
		return;
	}
	if (direction->cake_state == CAKE_OBSERVATION_AVAILABLE)
		direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
	direction->cake_valid = false;
	direction->next_cake_observation_microseconds = 0U;
	reset_traffic_observation(direction);
	monitor->links.qdisc_refresh = true;
}

static void handle_qdisc_events(struct uloop_fd *descriptor, unsigned int events)
{
	struct monitor *monitor =
		__extension__ container_of(descriptor, struct monitor, links.qdisc_events);
	char error[ERROR_SIZE] = { 0 };

	(void)events;
	if (netlink_receive_qdisc_events(&monitor->netlink, error, sizeof(error)) != 0) {
		log_message(LOG_LEVEL_ERROR, "qdisc lifecycle monitoring failed: %s", error);
		monitor->result = -1;
		uloop_end();
		return;
	}
	if (monitor->links.qdisc_refresh) {
		monitor->links.qdisc_refresh = false;
		monitor_tick(monitor);
	}
}

int links_watch_events(struct monitor *monitor)
{
	struct uloop_fd *events = &monitor->links.qdisc_events;
	char error[ERROR_SIZE] = { 0 };

	events->cb = handle_qdisc_events;
	if (netlink_subscribe_qdiscs(&monitor->netlink, process_qdisc_event, monitor, error,
				     sizeof(error)) != 0) {
		log_message(LOG_LEVEL_ERROR, "%s", error);
		return -1;
	}
	events->fd = netlink_event_descriptor(&monitor->netlink);
	if (events->fd < 0 || uloop_fd_add(events, ULOOP_READ | ULOOP_ERROR_CB) != 0) {
		log_message(LOG_LEVEL_ERROR, "could not monitor qdisc lifecycle: %s",
			    strerror(errno));
		return -1;
	}
	return 0;
}
