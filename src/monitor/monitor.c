#define _GNU_SOURCE

#include "monitor/monitor.h"
#include "monitor/loop.h"

#include "common/helpers.h"
#include "common/utils.h"
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

static void update_activity(struct monitor *monitor, uint64_t timestamp_us)
{
	static const char *const names[] = {
		STATE_RUNNING_UPPER,
		STATE_IDLE_UPPER,
		STATE_STALL_UPPER,
	};
	struct controller_activity *activity = &monitor->activity;
	const struct monitor_pingers *pingers = &monitor->pingers;
	const struct monitor_links *links = &monitor->links;
	const struct monitor_direction *download = &links->download;
	const struct monitor_direction *upload = &links->upload;
	const struct config *config = monitor->config;
	const struct controller_activity_config *activity_config = &activity->config;
	const struct controller_activity_input input = {
		.download = { .valid = download->traffic_valid,
			      .traffic_rate_bps = download->traffic_rate_bps },
		.upload = { .valid = upload->traffic_valid,
			    .traffic_rate_bps = upload->traffic_rate_bps },
		.timestamp_us = timestamp_us,
		.last_response_us = pingers->last_response_us,
		.last_pinger_start_us = pingers->last_restart_us,
		.grace_until_us = pingers->grace_until_us,
	};
	enum controller_activity_state previous = activity->state;
	struct controller_activity_output output;

	activity_update(activity, &input, &output);
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
		if (activity->state == CONTROLLER_RUNNING) {
			log_message(
				LOG_LEVEL_DEBUG,
				"load above connection stall threshold so resuming normal operation."
			);
		}
	}
	if (output.global_timeout_started) {
		if (config->minimum_shaper_rates_enforcement)
			control_enforce_minimum(monitor, timestamp_us);
		log_system_message(
			"Warning: Configured global ping response timeout: %.3f seconds exceeded.",
			us_to_sec_decimal(activity_config->global_timeout_us)
		);
	}
	if (output.state_changed) {
		if (activity->state == CONTROLLER_RUNNING) {
			log_message(
				LOG_LEVEL_DEBUG,
				previous == CONTROLLER_IDLE ?
					"Connection load exceeded active threshold. Resuming normal operation." :
					"Connection stall ended. Resuming normal operation."
			);
		}
		log_message(
			LOG_LEVEL_DEBUG,
			"Changing main state from: %s to: %s",
			names[previous],
			names[activity->state]
		);
		if (activity->state == CONTROLLER_IDLE) {
			log_message(LOG_LEVEL_DEBUG, "Connection idle. Waiting for minimum load.");
			if (config->minimum_shaper_rates_enforcement) {
				log_message(LOG_LEVEL_DEBUG, "Enforcing minimum shaper rates.");
				control_enforce_minimum(monitor, timestamp_us);
			}
			pingers_close(monitor);
		} else if (previous == CONTROLLER_IDLE) {
			pingers_resume(monitor, timestamp_us);
		}
	}
	if (output.restart_pingers) {
		log_message(LOG_LEVEL_DEBUG, "Restarting pingers.");
		pingers_restart(monitor, timestamp_us);
	}
}

/* One traffic cycle: observe both links, update the activity state, keep pingers running. */
void monitor_tick(struct monitor *monitor)
{
	const struct controller_activity *activity = &monitor->activity;
	uint64_t timestamp_us;

	links_observe(monitor);
	links_apply_cadence(monitor);
	tcp_drain(monitor);
	if (!links_ready(monitor)) {
		pingers_suspend(monitor);
		return;
	}
	if (read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		pingers_unsuspend(monitor, timestamp_us);
		update_activity(monitor, timestamp_us);
	}
	if (activity->state != CONTROLLER_IDLE)
		(void)pingers_watch(monitor);
}

static void handle_traffic_timer(struct uloop_interval *timer)
{
	struct monitor *monitor = __extension__ container_of(timer, struct monitor, traffic_timer);

	monitor_tick(monitor);
	/* After the tick, so also while the links are not ready. */
	publish_update(monitor);
}

static void start_activity(struct monitor *monitor)
{
	const struct config *config = monitor->config;
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

	activity_init(&monitor->activity, &activity_config);
}

int monitor_run(const struct config *config, const char *section_name)
{
	struct monitor monitor = {
		.config = config,
		.pinger_method = latency_method(config->pinger_method),
		.links = { .download = { .name = DIRECTION_DOWNLOAD,
					 .interface = config->ingress_interface,
					 .cake_state = CAKE_OBSERVATION_UNKNOWN,
					 .traffic_state = TRAFFIC_OBSERVATION_UNKNOWN },
			   .upload = { .name = DIRECTION_UPLOAD,
				       .interface = config->interface,
				       .cake_state = CAKE_OBSERVATION_UNKNOWN,
				       .traffic_state = TRAFFIC_OBSERVATION_UNKNOWN },
			   .qdisc_events = { .fd = -1 } },
		.traffic_timer = { .cb = handle_traffic_timer },
		.result = -1,
	};
	const struct {
		struct uloop_interval *timer;
		uint64_t interval_us;
		const char *name;
	} required_timers[] = {
		{ &monitor.traffic_timer,
		  config->monitor_achieved_rates_interval_us,
		  TIMER_TRAFFIC },
		{ &monitor.reflectors.health_timer,
		  config->reflector_health_check_interval_us,
		  TIMER_REFLECTOR_HEALTH },
	};
	int run_status;
	uint64_t start_us;
	size_t index;

	/* The aggregate initializer has zeroed the remaining monitor state. */
	upkeep_init(&monitor);
	tcp_init(&monitor);
	if (control_start(&monitor) != 0)
		goto done;
	/*
	 * Loading the TCP filter can take seconds on slow CPUs; it runs before the
	 * start time and timers, so reflector health and pinger grace start after it.
	 */
	tcp_start(&monitor);
	if (!read_clock_us(CLOCK_MONOTONIC, &start_us)) {
		log_message(
			LOG_LEVEL_ERROR,
			"could not initialize reflector health clock: %s",
			strerror(errno)
		);
		goto done;
	}
	if (reflectors_start(&monitor, start_us) != 0)
		goto done;
	pingers_prepare(&monitor, start_us);
	start_activity(&monitor);

	/* uloop reaps pinger children and reports each exit to pingers.c. */
	if (uloop_init() != 0) {
		log_message(LOG_LEVEL_ERROR, "could not initialize event loop: %s", strerror(errno));
		goto done;
	}

	if (links_watch_events(&monitor) != 0)
		goto uloop_done;
	/* After uloop_init(): the ubus thread relies on it; see publish.c. */
	publish_start(&monitor, section_name);

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

	links_observe(&monitor);
	links_apply_cadence(&monitor);
	upkeep_start(&monitor);
	(void)pingers_watch(&monitor);
	run_status = uloop_run();
	if (run_status == SIGINT || run_status == SIGTERM) {
		log_message(LOG_LEVEL_NOTICE, "received signal %d; shutting down", run_status);
		monitor.result = 0;
	}

uloop_done:
	pingers_stop_now(&monitor);
	if (monitor.links.qdisc_events.registered)
		(void)uloop_fd_delete(&monitor.links.qdisc_events);
	(void)uloop_interval_cancel(&monitor.traffic_timer);
	(void)uloop_interval_cancel(&monitor.reflectors.health_timer);
	upkeep_stop(&monitor);
	publish_stop(&monitor);
	uloop_done();

done:
	tcp_stop(&monitor);
	reflectors_stop(&monitor);
	netlink_close(&monitor.netlink);
	controller_close(&monitor.control.controller);
	return monitor.result;
}
