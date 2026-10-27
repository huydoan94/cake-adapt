#define _GNU_SOURCE

#include "daemon/daemon.h"
#include "daemon/report.h"
#include "daemon/state.h"

#include "common/arith.h"
#include "common/clock.h"
#include "common/convert.h"
#include "config/defaults.h"
#include "logging/log.h"
#include "constants/states.h"
#include "constants/text.h"
#include "constants/units.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <string.h>

static void update_activity(struct daemon *daemon, uint64_t timestamp_us)
{
	struct controller_activity *activity = &daemon->activity;
	const struct daemon_pingers *pingers = &daemon->pingers;
	const struct daemon_links *links = &daemon->links;
	const struct links_direction *download = &links->directions[DIRECTION_DOWNLOAD];
	const struct links_direction *upload = &links->directions[DIRECTION_UPLOAD];
	const struct config *config = daemon->config;
	const struct controller_activity_config *activity_config = &activity->config;
	struct controller_activity_input input = {
		.timestamp_us = timestamp_us,
		.last_response_us = pingers->last_response_us,
		.last_pinger_start_us = pingers->last_restart_us,
		.grace_until_us = pingers->grace_until_us,
	};
	enum controller_activity_state previous = activity->state;
	struct controller_activity_output output;

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
		const struct links_direction *link = &links->directions[direction];

		input.directions[direction] = (struct controller_direction_input){
			.valid = link->traffic_valid,
			.traffic_rate_bps = link->traffic_rate_bps,
		};
	}
	controller_activity_update(activity, &input, &output);
	if (output.check_stall_loads) {
		log_message(
			LOG_LEVEL_DEBUG,
			"Warning: no reflector response within: %.2f seconds. Checking loads.",
			us_to_sec_decimal(activity_config->stall_timeout_us)
		);
		log_message(
			LOG_LEVEL_DEBUG,
			"load check is: (( %" PRIu64 " kbps > %" PRIu64
			" kbps for download && %" PRIu64 " kbps > %" PRIu64 " kbps for upload ))",
			bit_to_kbit(download->traffic_rate_bps),
			bit_to_kbit(config->connection_stall_threshold_bps),
			bit_to_kbit(upload->traffic_rate_bps),
			bit_to_kbit(config->connection_stall_threshold_bps)
		);
		if (activity->state == CONTROLLER_ACTIVITY_RUNNING) {
			log_message(
				LOG_LEVEL_DEBUG,
				"load above connection stall threshold so resuming normal operation."
			);
		}
	}
	if (output.global_timeout_started) {
		if (config->minimum_shaper_rates_enforcement)
			shaper_enforce_minimum(daemon, timestamp_us);
		log_system_message(
			"Warning: Configured global ping response timeout: %.3f seconds exceeded.",
			us_to_sec_decimal(activity_config->global_timeout_us)
		);
	}
	if (output.state_changed) {
		if (activity->state == CONTROLLER_ACTIVITY_RUNNING) {
			log_message(
				LOG_LEVEL_DEBUG,
				previous == CONTROLLER_ACTIVITY_IDLE ?
					"Connection load exceeded active threshold. Resuming normal operation." :
					"Connection stall ended. Resuming normal operation."
			);
		}
		log_message(
			LOG_LEVEL_DEBUG,
			"Changing main state from: %s to: %s",
			report_activity_log_name(previous),
			report_activity_log_name(activity->state)
		);
		if (activity->state == CONTROLLER_ACTIVITY_IDLE) {
			log_message(LOG_LEVEL_DEBUG, "Connection idle. Waiting for minimum load.");
			if (config->minimum_shaper_rates_enforcement) {
				log_message(LOG_LEVEL_DEBUG, "Enforcing minimum shaper rates.");
				shaper_enforce_minimum(daemon, timestamp_us);
			}
			pingers_close(daemon);
		} else if (previous == CONTROLLER_ACTIVITY_IDLE) {
			pingers_resume(daemon, timestamp_us);
		}
	}
	if (output.restart_pingers) {
		log_message(LOG_LEVEL_DEBUG, "Restarting pingers.");
		pingers_restart(daemon, timestamp_us);
	}
}

/* One traffic cycle: observe both links, update the activity state, keep pingers running. */
void daemon_tick(struct daemon *daemon)
{
	const struct controller_activity *activity = &daemon->activity;
	uint64_t timestamp_us;

	links_observe(daemon);
	links_apply_cadence(daemon);
	tcp_drain(daemon);
	if (!links_ready(daemon)) {
		pingers_suspend(daemon);
		return;
	}
	if (read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		pingers_unsuspend(daemon, timestamp_us);
		update_activity(daemon, timestamp_us);
	}
	if (activity->state != CONTROLLER_ACTIVITY_IDLE)
		(void)pingers_watch(daemon);
}

static void handle_traffic_timer(struct uloop_interval *timer)
{
	struct daemon *daemon = __extension__ container_of(timer, struct daemon, traffic_timer);

	daemon_tick(daemon);
	/* After the tick, so also while the links are not ready. */
	publish_update(daemon);
}

static void start_activity(struct daemon *daemon)
{
	const struct config *config = daemon->config;
	const struct controller_activity_config activity_config = {
		.enable_sleep = config->enable_sleep_function,
		.active_threshold_bps = config->connection_active_threshold_bps,
		.stall_threshold_bps = config->connection_stall_threshold_bps,
		.sustained_idle_us = config->sustained_idle_sleep_threshold_us,
		.stall_timeout_us =
			config->stall_detection_threshold *
			rounded_divide(config->reflector_ping_interval_us, config->no_pingers),
		.global_timeout_us = config->global_ping_response_timeout_us,
	};

	controller_activity_init(&daemon->activity, &activity_config);
}

int daemon_run(const struct config *config, const char *section_name)
{
	struct daemon daemon = {
		.config = config,
		.pinger_method = pinger_method_parse(config->pinger_method),
		.links = { .qdisc_events = { .fd = -1 } },
		.traffic_timer = { .cb = handle_traffic_timer },
		.result = -1,
	};
	const struct {
		struct uloop_interval *timer;
		uint64_t interval_us;
		const char *name;
	} required_timers[] = {
		{ &daemon.traffic_timer, config->monitor_achieved_rates_interval_us, TIMER_TRAFFIC },
		{ &daemon.reflectors.health_timer,
		  config->reflector_health_check_interval_us,
		  TIMER_REFLECTOR_HEALTH },
	};
	/* Download is shaped on the IFB, after the bottleneck; upload on the WAN interface. */
	const char *const interfaces[DIRECTION_COUNT] = {
		[DIRECTION_DOWNLOAD] = config->ingress_interface,
		[DIRECTION_UPLOAD] = config->interface,
	};
	struct daemon_links *links = &daemon.links;
	int run_status;
	uint64_t start_us;
	size_t index;

	/* The aggregate initializer has zeroed the remaining daemon state. */
	for (index = 0U; index < DIRECTION_COUNT; index++) {
		struct links_direction *link = &links->directions[index];

		link->name = direction_name((enum direction)index);
		link->interface = interfaces[index];
		link->cake_state = LINKS_CAKE_UNKNOWN;
		link->traffic_state = LINKS_TRAFFIC_UNKNOWN;
	}
	upkeep_init(&daemon);
	tcp_init(&daemon);
	if (shaper_start(&daemon) != 0)
		goto done;
	/*
	 * Loading the TCP filter can take seconds on slow CPUs; it runs before the
	 * start time and timers, so reflector health and pinger grace start after it.
	 */
	tcp_start(&daemon);
	if (!read_clock_us(CLOCK_MONOTONIC, &start_us)) {
		log_message(
			LOG_LEVEL_ERROR,
			"could not initialize reflector health clock: %s",
			strerror(errno)
		);
		goto done;
	}
	if (reflectors_start(&daemon, start_us) != 0)
		goto done;
	pingers_prepare(&daemon, start_us);
	start_activity(&daemon);

	/* uloop reaps pinger children and reports each exit to pingers.c. */
	if (uloop_init() != 0) {
		log_message(LOG_LEVEL_ERROR, "could not initialize event loop: %s", strerror(errno));
		goto done;
	}

	if (links_watch_events(&daemon) != 0)
		goto uloop_done;
	/* After uloop_init(): the ubus thread relies on it; see publish.c. */
	publish_start(&daemon, section_name);

	for (index = 0; index < ARRAY_SIZE(required_timers); ++index) {
		if (uloop_interval_set(
			    required_timers[index].timer,
			    us_to_ms(required_timers[index].interval_us)
		    ) != 0) {
			log_message(
				LOG_LEVEL_ERROR,
				"could not monitor %s timer: %s",
				required_timers[index].name,
				strerror(errno)
			);
			goto uloop_done;
		}
	}

	links_observe(&daemon);
	links_apply_cadence(&daemon);
	upkeep_start(&daemon);
	(void)pingers_watch(&daemon);
	run_status = uloop_run();
	if (run_status == SIGINT || run_status == SIGTERM) {
		log_message(LOG_LEVEL_NOTICE, "received signal %d; shutting down", run_status);
		daemon.result = 0;
	}

uloop_done:
	pingers_stop_now(&daemon);
	if (daemon.links.qdisc_events.registered)
		(void)uloop_fd_delete(&daemon.links.qdisc_events);
	(void)uloop_interval_cancel(&daemon.traffic_timer);
	(void)uloop_interval_cancel(&daemon.reflectors.health_timer);
	upkeep_stop(&daemon);
	publish_stop(&daemon);
	uloop_done();

done:
	tcp_stop(&daemon);
	reflectors_stop(&daemon);
	netlink_close(&daemon.netlink);
	controller_close(&daemon.shaper.controller);
	return daemon.result;
}
