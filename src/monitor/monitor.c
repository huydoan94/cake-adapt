#define _GNU_SOURCE

#include "monitor/monitor.h"
#include "monitor/loop.h"

#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <string.h>

static void update_activity(struct monitor *monitor, uint64_t timestamp_microseconds)
{
	static const char *const names[] = {
		STATE_RUNNING_UPPER,
		STATE_IDLE_UPPER,
		STATE_STALL_UPPER,
	};
	const struct monitor_links *links = &monitor->links;
	const struct config *config = monitor->config;
	const struct controller_activity_config *activity_config = &monitor->activity.config;
	const struct controller_activity_input input = {
		.download = { .valid = links->download.traffic_valid,
			      .traffic_rate_bits_per_second =
				      links->download.traffic_rate_bits_per_second },
		.upload = { .valid = links->upload.traffic_valid,
			    .traffic_rate_bits_per_second =
				    links->upload.traffic_rate_bits_per_second },
		.timestamp_microseconds = timestamp_microseconds,
		.last_response_microseconds = monitor->pingers.last_response_microseconds,
		.last_pinger_start_microseconds = monitor->pingers.last_restart_microseconds,
		.grace_until_microseconds = monitor->pingers.grace_until_microseconds,
	};
	enum controller_activity_state previous = monitor->activity.state;
	struct controller_activity_output output;

	activity_update(&monitor->activity, &input, &output);
	if (output.check_stall_loads) {
		log_message(
			LOG_LEVEL_DEBUG,
			"Warning: no reflector response within: %.2f seconds. Checking loads.",
			seconds_from_microseconds(activity_config->stall_timeout_microseconds)
		);
		log_message(
			LOG_LEVEL_DEBUG,
			"load check is: (( %" PRIu64 " kbps > %" PRIu64
			" kbps for download && %" PRIu64 " kbps > %" PRIu64 " kbps for upload ))",
			links->download.traffic_rate_bits_per_second / KILOBIT,
			config->connection_stall_threshold_bits_per_second / KILOBIT,
			links->upload.traffic_rate_bits_per_second / KILOBIT,
			config->connection_stall_threshold_bits_per_second / KILOBIT
		);
		if (monitor->activity.state == CONTROLLER_RUNNING) {
			log_message(
				LOG_LEVEL_DEBUG,
				"load above connection stall threshold so resuming normal operation."
			);
		}
	}
	if (output.global_timeout_started) {
		if (config->minimum_shaper_rates_enforcement)
			control_enforce_minimum(monitor, timestamp_microseconds);
		log_system_message(
			"Warning: Configured global ping response timeout: %.3f seconds exceeded.",
			seconds_from_microseconds(activity_config->global_timeout_microseconds)
		);
	}
	if (output.state_changed) {
		if (monitor->activity.state == CONTROLLER_RUNNING) {
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
			names[monitor->activity.state]
		);
		if (monitor->activity.state == CONTROLLER_IDLE) {
			log_message(LOG_LEVEL_DEBUG, "Connection idle. Waiting for minimum load.");
			if (config->minimum_shaper_rates_enforcement) {
				log_message(LOG_LEVEL_DEBUG, "Enforcing minimum shaper rates.");
				control_enforce_minimum(monitor, timestamp_microseconds);
			}
			pingers_close(monitor);
		} else if (previous == CONTROLLER_IDLE) {
			pingers_resume(monitor, timestamp_microseconds);
		}
	}
	if (output.restart_pingers) {
		log_message(LOG_LEVEL_DEBUG, "Restarting pingers.");
		pingers_restart(monitor, timestamp_microseconds);
	}
}

/* One traffic cycle: observe both links, update the activity state, keep pingers running. */
void monitor_tick(struct monitor *monitor)
{
	uint64_t timestamp_microseconds;

	links_observe(monitor);
	links_apply_cadence(monitor);
	tcp_drain(monitor);
	if (!links_ready(monitor)) {
		pingers_suspend(monitor);
		return;
	}
	if (read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
		pingers_unsuspend(monitor, timestamp_microseconds);
		update_activity(monitor, timestamp_microseconds);
	}
	if (monitor->activity.state != CONTROLLER_IDLE)
		(void)pingers_watch(monitor);
}

static void handle_traffic_timer(struct uloop_interval *timer)
{
	monitor_tick(__extension__ container_of(timer, struct monitor, traffic_timer));
}

/*
 * Reads the counters and reports failure, recovery and a changed CPU count.
 * cpu_read() initializes only the counters the kernel returns.
 */
static bool read_cpu(struct monitor *monitor, struct cpu_sample *sample)
{
	char error[ERROR_SIZE] = { 0 };

	if (cpu_read(PROC_STAT_PATH, sample, error, sizeof(error)) != 0) {
		if (!monitor->cpu_observation_failed)
			log_message(LOG_LEVEL_WARNING, "CPU observation degraded: %s", error);
		monitor->cpu_observation_failed = true;
		return false;
	}
	if (monitor->cpu_observation_failed) {
		log_message(LOG_LEVEL_NOTICE, "CPU observation recovered");
		monitor->cpu_observation_failed = false;
	}
	if (monitor->cpu_count != sample->count) {
		monitor->cpu_count = sample->count;
		cpu_init(&monitor->cpu_monitor);
		log_message(LOG_LEVEL_DEBUG, "Detected %zu CPU cores.", sample->count - 1U);
		log_print_cpu_headers(
			sample,
			monitor->config->output_cpu_stats,
			monitor->config->output_cpu_raw_stats
		);
	}
	return true;
}

static void handle_cpu_timer(struct uloop_interval *timer)
{
	struct monitor *monitor = __extension__ container_of(timer, struct monitor, cpu_timer);
	struct cpu_sample sample;
	unsigned int usage[CPU_MAX_COUNT];

	if (monitor->activity.state != CONTROLLER_RUNNING || !read_cpu(monitor, &sample))
		return;
	if (monitor->config->output_cpu_raw_stats)
		log_cpu_raw(&sample);
	if (monitor->config->output_cpu_stats) {
		cpu_usage(&monitor->cpu_monitor, &sample, usage);
		log_cpu(&sample, usage);
	}
}

static void handle_log_timer(struct uloop_interval *timer)
{
	(void)timer;
	log_tick();
}

static void handle_log_export_signal(struct uloop_signal *signal)
{
	char export_path[LOG_EXPORT_PATH_SIZE];

	(void)signal;
	log_message(LOG_LEVEL_DEBUG, "received log file export signal so exporting log file.");
	if (log_export_file(export_path, sizeof(export_path)) != 0)
		log_message(LOG_LEVEL_WARNING, "log file export failed: %s", strerror(errno));
}

static void handle_log_reset_signal(struct uloop_signal *signal)
{
	(void)signal;
	log_message(
		LOG_LEVEL_DEBUG,
		"received log file reset signal so flushing log and resetting log file."
	);
	if (log_reset_file() != 0)
		log_message(LOG_LEVEL_WARNING, "log file reset failed: %s", strerror(errno));
}

static void watch_cpu(struct monitor *monitor)
{
	const struct config *config = monitor->config;

	struct cpu_sample sample;

	if (!config->output_cpu_stats && !config->output_cpu_raw_stats)
		return;
	/* Detect the CPUs and print the headers now; records start with the timer. */
	(void)read_cpu(monitor, &sample);
	if (uloop_interval_set(
		    &monitor->cpu_timer,
		    timer_milliseconds(config->monitor_cpu_usage_interval_microseconds)
	    ) != 0) {
		log_message(LOG_LEVEL_WARNING, "could not monitor CPU timer: %s", strerror(errno));
	}
}

static void watch_log_maintenance(struct monitor *monitor)
{
	const struct config *config = monitor->config;
	uint64_t log_timer_milliseconds;

	if (!config->log_to_file)
		return;
	log_timer_milliseconds =
		milliseconds_rounded_up(config->log_file_buffer_timeout_microseconds);
	/* Without a buffer timer, still wake up to rotate at the maximum age. */
	if (log_timer_milliseconds == 0U && config->log_file_max_time_minutes > 0U) {
		uint64_t maximum_age_milliseconds =
			config->log_file_max_time_minutes * (MINUTE / MILLISECOND);

		log_timer_milliseconds = maximum_age_milliseconds < UINT_MAX ?
						 maximum_age_milliseconds + 1U :
						 UINT_MAX;
	}
	if (log_timer_milliseconds > 0U &&
	    uloop_interval_set(&monitor->log_timer, (unsigned int)log_timer_milliseconds) != 0) {
		log_message(LOG_LEVEL_WARNING, "log timer degraded: %s", strerror(errno));
	}
	if (uloop_signal_add(&monitor->log_export_signal) != 0)
		log_message(LOG_LEVEL_WARNING, "log export signal degraded: %s", strerror(errno));
	if (uloop_signal_add(&monitor->log_reset_signal) != 0)
		log_message(LOG_LEVEL_WARNING, "log reset signal degraded: %s", strerror(errno));
}

static void start_activity(struct monitor *monitor)
{
	const struct config *config = monitor->config;
	const struct controller_activity_config activity_config = {
		.enable_sleep = config->enable_sleep_function,
		.active_threshold_bits_per_second =
			config->connection_active_threshold_bits_per_second,
		.stall_threshold_bits_per_second =
			config->connection_stall_threshold_bits_per_second,
		.sustained_idle_microseconds = config->sustained_idle_sleep_threshold_microseconds,
		.stall_timeout_microseconds =
			config->stall_detection_threshold *
			(config->reflector_ping_interval_microseconds / config->no_pingers),
		.global_timeout_microseconds = config->global_ping_response_timeout_microseconds,
	};

	activity_init(&monitor->activity, &activity_config);
}

int monitor_run(const struct config *config)
{
	struct monitor monitor = {
		.config = config,
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
		.cpu_timer = { .cb = handle_cpu_timer },
		.log_timer = { .cb = handle_log_timer },
		.log_export_signal = { .cb = handle_log_export_signal, .signo = SIGUSR1 },
		.log_reset_signal = { .cb = handle_log_reset_signal, .signo = SIGUSR2 },
		.result = -1,
	};
	const struct {
		struct uloop_interval *timer;
		uint64_t interval_microseconds;
		const char *name;
	} required_timers[] = {
		{ &monitor.traffic_timer,
		  config->monitor_achieved_rates_interval_microseconds,
		  TIMER_TRAFFIC },
		{ &monitor.reflectors.health_timer,
		  config->reflector_health_check_interval_microseconds,
		  TIMER_REFLECTOR_HEALTH },
	};
	int run_status;
	uint64_t start_microseconds;
	size_t index;

	/* The aggregate initializer has zeroed the remaining monitor state. */
	if (control_start(&monitor) != 0)
		goto done;
	if (!read_clock_microseconds(CLOCK_MONOTONIC, &start_microseconds)) {
		log_message(
			LOG_LEVEL_ERROR,
			"could not initialize reflector health clock: %s",
			strerror(errno)
		);
		goto done;
	}
	if (reflectors_start(&monitor, start_microseconds) != 0)
		goto done;
	pingers_prepare(&monitor, start_microseconds);
	start_activity(&monitor);

	/* uloop reaps pinger children and reports each exit to pingers.c. */
	if (uloop_init() != 0) {
		log_message(LOG_LEVEL_ERROR, "could not initialize event loop: %s", strerror(errno));
		goto done;
	}

	if (links_watch_events(&monitor) != 0)
		goto uloop_done;

	for (index = 0; index < ARRAY_SIZE(required_timers); ++index) {
		if (uloop_interval_set(
			    required_timers[index].timer,
			    timer_milliseconds(required_timers[index].interval_microseconds)
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
	watch_cpu(&monitor);
	watch_log_maintenance(&monitor);
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
	(void)uloop_interval_cancel(&monitor.cpu_timer);
	(void)uloop_interval_cancel(&monitor.log_timer);
	(void)uloop_signal_delete(&monitor.log_export_signal);
	(void)uloop_signal_delete(&monitor.log_reset_signal);
	uloop_done();

done:
	tcp_close(&monitor);
	reflectors_stop(&monitor);
	netlink_close(&monitor.netlink);
	controller_close(&monitor.control.controller);
	return monitor.result;
}
