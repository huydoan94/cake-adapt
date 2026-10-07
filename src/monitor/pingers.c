#define _GNU_SOURCE

#include "monitor/loop.h"

#include "config/defaults.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool schedule_irtt_child_start(struct monitor *monitor);

static void report_latency_degraded(struct monitor *monitor, const char *error)
{
	struct monitor_pingers *pingers = &monitor->pingers;

	if (!pingers->observation_failed)
		log_message(LOG_LEVEL_WARNING, "latency observation degraded: %s", error);
	pingers->observation_failed = true;
}

static void report_latency_failure(struct monitor *monitor, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

static void report_latency_failure(struct monitor *monitor, const char *format, ...)
{
	char error[ERROR_SIZE];
	va_list arguments;

	va_start(arguments, format);
	(void)vsnprintf(error, sizeof(error), format, arguments);
	va_end(arguments);
	report_latency_degraded(monitor, error);
}

/* Like interface checks, a failed pinger start is retried after interface_up_check_interval_s. */
static void schedule_retry(struct monitor *monitor, uint64_t timestamp_us)
{
	monitor->pingers.next_attempt_us =
		timestamp_us + monitor->config->interface_up_check_interval_us;
}

static bool ensure_latency_open(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	struct latency *latency = &pingers->latency;
	const struct config *config = monitor->config;
	bool irtt = strcmp(config->pinger_method, PINGER_METHOD_IRTT) == 0;
	const char *targets[CONFIG_MAX_REFLECTORS];
	char error[ERROR_SIZE] = { 0 };
	size_t target_count = (size_t)config->no_pingers;
	uint64_t timestamp_us;
	enum log_level level;
	const char *outcome;

	if (!links_ready(monitor))
		return false;
	if (latency_is_open(latency))
		return true;
	/* A new session waits for the previous pingers to be reaped. */
	if (latency_stopping(latency))
		return false;
	if (!read_clock_us(CLOCK_MONOTONIC, &timestamp_us) ||
	    timestamp_us < pingers->next_attempt_us) {
		return false;
	}
	schedule_retry(monitor, timestamp_us);
	reflectors_active(monitor, targets);
	if (latency_open(latency, targets, target_count, timestamp_us, error, sizeof(error)) != 0) {
		report_latency_degraded(monitor, error);
		return false;
	}

	level = pingers->observation_failed ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO;
	outcome = pingers->observation_failed ? STATE_RECOVERED : STATE_INITIALIZED;
	if (irtt) {
		log_message(
			level,
			"latency observation %s: targets=%zu pinger=irtt",
			outcome,
			target_count
		);
	} else {
		log_message(
			level,
			"latency observation %s: targets=%zu interface=%s",
			outcome,
			target_count,
			config->interface
		);
	}
	pingers->observation_failed = false;
	pingers->next_attempt_us = 0U;
	pingers->last_restart_us = timestamp_us;
	return true;
}

/* Returns false when latency observation must be restarted. */
static bool process_latency_line(struct monitor *monitor, size_t child_index, const char *line)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	struct latency_observation observation;
	struct latency_sample sample;
	char error[ERROR_SIZE] = { 0 };
	uint64_t processing_realtime_us;
	uint64_t processing_monotonic_us;
	uint64_t response_us;
	enum latency_probe_result result = latency_handle_line(
		&pingers->latency,
		child_index,
		line,
		&sample,
		error,
		sizeof(error)
	);
	size_t slot;

	if (result == LATENCY_PROBE_PENDING || result == LATENCY_PROBE_TIMEOUT)
		return true;
	if (result == LATENCY_PROBE_ERROR) {
		report_latency_degraded(monitor, error);
		return false;
	}

	slot = reflectors_find(monitor, sample.target);
	if (slot == SIZE_MAX) {
		report_latency_failure(monitor, "unexpected reflector=%s", sample.target);
		return false;
	}

	if (!read_clock_us(CLOCK_REALTIME, &processing_realtime_us) ||
	    !read_clock_us(CLOCK_MONOTONIC, &processing_monotonic_us)) {
		if (!pingers->response_clock_failed) {
			log_message(
				LOG_LEVEL_WARNING,
				"latency observation degraded: response clock failed: %s",
				strerror(errno)
			);
		}
		pingers->response_clock_failed = true;
		return true;
	}
	if (pingers->response_clock_failed) {
		log_message(LOG_LEVEL_NOTICE, "latency response clock recovered");
		pingers->response_clock_failed = false;
	}
	response_us = response_monotonic_us(
		processing_realtime_us,
		processing_monotonic_us,
		sample.timestamp_us
	);
	reflectors_record(monitor, slot, &sample, response_us, &observation);
	pingers->last_response_us = response_us;
	if (response_stale(processing_realtime_us, sample.timestamp_us)) {
		log_message(
			LOG_LEVEL_DEBUG,
			"processed response from [%s] that is > 500ms old. Skipping.",
			sample.target
		);
		return true;
	}

	control_update(monitor, &observation, &sample);
	return true;
}

/* Every spawned child must be known to uloop before anything can stop it. */
static void register_pinger_processes(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	size_t index;

	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct pinger_watch *watch = &pingers->watches[index];
		pid_t process_identifier = latency_child_process(&pingers->latency, index);

		if (process_identifier > 0 && !watch->process.pending) {
			watch->process.pid = process_identifier;
			(void)uloop_process_add(&watch->process);
		}
	}
}

static void release_pinger_output(struct pinger_watch *watch)
{
	if (watch->reading) {
		ustream_free(&watch->output.stream);
		watch->reading = false;
	}
}

void pingers_close(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	size_t index;

	(void)uloop_timeout_cancel(&pingers->start_timer);
	register_pinger_processes(monitor);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++)
		release_pinger_output(&pingers->watches[index]);
	latency_close(&pingers->latency);
	/* uloop reaps the children; escalate once if SIGTERM is ignored. */
	if (latency_stopping(&pingers->latency) && !pingers->stop_timer.pending)
		(void)uloop_timeout_set(
			&pingers->stop_timer,
			(int)us_to_millisec(CHILD_STOP_TIMEOUT_US)
		);
}

/* The event loop has stopped, so pingers are stopped and reaped synchronously. */
void pingers_stop_now(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	size_t index;

	(void)uloop_timeout_cancel(&pingers->start_timer);
	(void)uloop_timeout_cancel(&pingers->stop_timer);
	(void)uloop_timeout_cancel(&pingers->failure_timer);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct pinger_watch *watch = &pingers->watches[index];

		release_pinger_output(watch);
		(void)uloop_process_delete(&watch->process);
	}
	latency_stop_now(&pingers->latency);
}

static void handle_pinger_stop_timeout(struct uloop_timeout *timer)
{
	struct monitor *monitor =
		__extension__ container_of(timer, struct monitor, pingers.stop_timer);

	log_message(
		LOG_LEVEL_WARNING,
		"%s did not stop within %u ms of SIGTERM; sending SIGKILL",
		monitor->config->pinger_method,
		us_to_millisec(CHILD_STOP_TIMEOUT_US)
	);
	latency_kill_stopping(&monitor->pingers.latency);
}

static void defer_latency_retry(struct monitor *monitor)
{
	uint64_t timestamp_us;

	pingers_close(monitor);
	if (read_clock_us(CLOCK_MONOTONIC, &timestamp_us))
		schedule_retry(monitor, timestamp_us);
}

static void handle_latency_failure(struct uloop_timeout *timer)
{
	struct monitor *monitor =
		__extension__ container_of(timer, struct monitor, pingers.failure_timer);

	defer_latency_retry(monitor);
}

static void handle_pinger_output(struct ustream *stream, int bytes)
{
	struct pinger_watch *watch =
		__extension__ container_of(stream, struct pinger_watch, output.stream);
	struct monitor *monitor = watch->monitor;

	(void)bytes;
	for (;;) {
		char line[LATENCY_OUTPUT_SIZE];
		size_t consumed;
		int length;
		const char *data = ustream_get_read_buf(stream, &length);
		enum latency_line_result result;

		if (data == NULL)
			return;
		result = latency_next_line(data, (size_t)length, line, &consumed);
		if (result == LATENCY_LINE_INCOMPLETE)
			return;
		if (result == LATENCY_LINE_TOO_LONG) {
			report_latency_failure(
				monitor,
				"%s output line is too long",
				monitor->config->pinger_method
			);
			break;
		}
		ustream_consume(stream, (int)consumed);
		if (!process_latency_line(monitor, watch->child_index, line))
			break;
	}
	/* A stream cannot be freed from its read callback; restart from the loop. */
	ustream_set_read_blocked(stream, true);
	(void)uloop_timeout_set(&monitor->pingers.failure_timer, 0);
}

/* End of output or a descriptor error; the exit callback reports the cause. */
static void handle_pinger_state(struct ustream *stream)
{
	struct pinger_watch *watch =
		__extension__ container_of(stream, struct pinger_watch, output.stream);

	release_pinger_output(watch);
}

static void handle_pinger_exit(struct uloop_process *process, int status)
{
	struct pinger_watch *watch =
		__extension__ container_of(process, struct pinger_watch, process);
	struct monitor *monitor = watch->monitor;
	struct monitor_pingers *pingers = &monitor->pingers;
	struct latency *latency = &pingers->latency;
	char error[ERROR_SIZE] = { 0 };

	/* Deliver output written just before the exit, then stop reading. */
	if (watch->reading) {
		(void)ustream_poll(&watch->output.stream);
		release_pinger_output(watch);
	}
	switch (latency_child_exited(latency, watch->child_index, status, error, sizeof(error))) {
	case LATENCY_PROBE_STOPPED:
		if (!latency_stopping(latency)) {
			(void)uloop_timeout_cancel(&pingers->stop_timer);
			/* Like cake-autorate, a restart starts once the old pingers are gone. */
			if (monitor->activity.state != CONTROLLER_IDLE)
				(void)pingers_watch(monitor);
		}
		return;
	case LATENCY_PROBE_RESTART:
		log_message(
			LOG_LEVEL_DEBUG,
			"Restarting irtt pinger: pinger=%zu (%s)",
			watch->child_index,
			error
		);
		(void)schedule_irtt_child_start(monitor);
		return;
	default:
		report_latency_degraded(monitor, error);
		defer_latency_retry(monitor);
		return;
	}
}

static bool watch_started_latency_children(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	const struct latency *latency = &pingers->latency;
	size_t index;

	register_pinger_processes(monitor);
	for (index = 0U; index < latency_child_count(latency); index++) {
		struct pinger_watch *watch = &pingers->watches[index];
		int descriptor = latency_child_descriptor(latency, index);

		if (watch->reading || descriptor < 0)
			continue;
		watch->output = (struct ustream_fd){ .stream = {
							     .notify_read = handle_pinger_output,
							     .notify_state = handle_pinger_state,
						     } };
		ustream_fd_init(&watch->output, descriptor);
		watch->reading = true;
		/* ustream_fd_init() does not report a failed registration itself. */
		if (!watch->output.fd.registered) {
			report_latency_failure(
				monitor,
				"could not monitor %s output",
				monitor->config->pinger_method
			);
			pingers_close(monitor);
			return false;
		}
	}
	return true;
}

static bool schedule_irtt_child_start(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	struct latency *latency = &pingers->latency;
	char error[ERROR_SIZE] = { 0 };
	uint64_t timestamp_us;
	uint64_t next_start_us;
	uint64_t delay_milliseconds;

	if (!latency_irtt_start_pending(latency)) {
		(void)uloop_timeout_cancel(&pingers->start_timer);
		return true;
	}
	if (!read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		report_latency_failure(monitor, "IRTT start clock failed: %s", strerror(errno));
		defer_latency_retry(monitor);
		return false;
	}
	if (latency_start_irtt_children(latency, timestamp_us, error, sizeof(error)) != 0) {
		report_latency_degraded(monitor, error);
		defer_latency_retry(monitor);
		return false;
	}
	if (!watch_started_latency_children(monitor))
		return false;
	if (!latency_irtt_start_pending(latency))
		return true;

	next_start_us = latency_irtt_next_start_us(latency);
	/* uloop timeouts take a signed int of milliseconds. */
	delay_milliseconds =
		min_u64(us_to_millisec(saturating_sub(next_start_us, timestamp_us)), INT_MAX);
	if (uloop_timeout_set(&pingers->start_timer, (int)delay_milliseconds) != 0) {
		report_latency_failure(
			monitor,
			"could not schedule IRTT start: %s",
			strerror(errno)
		);
		defer_latency_retry(monitor);
		return false;
	}
	return true;
}

static void handle_latency_start(struct uloop_timeout *timer)
{
	struct monitor *monitor =
		__extension__ container_of(timer, struct monitor, pingers.start_timer);

	(void)schedule_irtt_child_start(monitor);
}

bool pingers_watch(struct monitor *monitor)
{
	if (!ensure_latency_open(monitor))
		return false;
	if (strcmp(monitor->config->pinger_method, PINGER_METHOD_IRTT) == 0)
		return schedule_irtt_child_start(monitor);
	return watch_started_latency_children(monitor);
}

/* Match cake-autorate's two-period setup grace after (re)starting pingers. */
static void grant_grace(struct monitor *monitor, uint64_t timestamp_us)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	const struct config *config = monitor->config;

	pingers->last_response_us = timestamp_us;
	pingers->grace_until_us = timestamp_us + 2U * config->reflector_ping_interval_us;
	reflectors_reset_health(monitor, pingers->grace_until_us);
}

void pingers_suspend(struct monitor *monitor)
{
	pingers_close(monitor);
	monitor->pingers.suspended = true;
}

/* Pingers stopped while CAKE was missing restart like after IDLE. */
void pingers_unsuspend(struct monitor *monitor, uint64_t timestamp_us)
{
	struct monitor_pingers *pingers = &monitor->pingers;

	if (!pingers->suspended)
		return;
	grant_grace(monitor, timestamp_us);
	pingers->suspended = false;
}

void pingers_resume(struct monitor *monitor, uint64_t timestamp_us)
{
	grant_grace(monitor, timestamp_us);
	monitor->pingers.next_attempt_us = 0U;
	(void)pingers_watch(monitor);
}

void pingers_reopen(struct monitor *monitor)
{
	pingers_close(monitor);
	monitor->pingers.next_attempt_us = 0U;
	(void)pingers_watch(monitor);
}

void pingers_restart(struct monitor *monitor, uint64_t timestamp_us)
{
	reflectors_reset_health(monitor, timestamp_us);
	monitor->pingers.last_restart_us = timestamp_us;
	pingers_reopen(monitor);
}

/* Each slot and timer is bound to its handler before uloop starts. */
void pingers_prepare(struct monitor *monitor, uint64_t start_us)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	const struct config *config = monitor->config;
	const struct latency_settings settings = {
		.pinger_method = config->pinger_method,
		.interface = config->interface,
		.extra_arguments = config->ping_extra_args,
		.prefix = config->ping_prefix_string,
		.reflector_ping_interval_us = config->reflector_ping_interval_us,
		.irtt_session_duration_us = config->irtt_session_duration_us,
		.slot_origin_us = start_us,
	};
	size_t index;

	pingers->last_response_us = start_us;
	pingers->last_restart_us = start_us;
	pingers->start_timer.cb = handle_latency_start;
	pingers->stop_timer.cb = handle_pinger_stop_timeout;
	pingers->failure_timer.cb = handle_latency_failure;
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct pinger_watch *watch = &pingers->watches[index];

		watch->process.cb = handle_pinger_exit;
		watch->monitor = monitor;
		watch->child_index = index;
	}
	latency_init(&pingers->latency, &settings);
}
