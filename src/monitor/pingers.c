#define _GNU_SOURCE

#include "monitor/loop.h"

#include "config/defaults.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool schedule_irtt_child_start(struct event_loop *loop);

static void report_latency_degraded(struct observation_context *context, const char *error)
{
	if (!context->latency_observation_failed)
		log_message(LOG_LEVEL_WARNING, "latency observation degraded: %s", error);
	context->latency_observation_failed = true;
}

static bool ensure_latency_open(struct observation_context *context, const struct config *config)
{
	const char *targets[CONFIG_MAX_REFLECTORS];
	char error[ERROR_SIZE] = { 0 };
	size_t target_count = (size_t)config->no_pingers;
	size_t index;
	uint64_t timestamp_microseconds;
	uint64_t first_start_microseconds;
	enum log_level level;
	const char *outcome;
	int result;

	if (!cake_ready(context))
		return false;
	if (latency_is_open(&context->latency))
		return true;
	/* A new session waits for the previous pingers to be reaped. */
	if (latency_stopping(&context->latency))
		return false;
	if (!read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds) ||
	    timestamp_microseconds < context->next_latency_attempt_microseconds) {
		return false;
	}
	context->next_latency_attempt_microseconds =
		timestamp_microseconds + config->interface_up_check_interval_microseconds;
	for (index = 0U; index < target_count; index++)
		targets[index] = config->reflectors[context->reflector_order[index]];
	if (strcmp(config->pinger_method, PINGER_METHOD_IRTT) == 0) {
		uint64_t elapsed =
			timestamp_microseconds - context->pinger_slot_origin_microseconds;
		uint64_t remainder = elapsed % config->reflector_ping_interval_microseconds;

		first_start_microseconds = timestamp_microseconds +
					   config->reflector_ping_interval_microseconds - remainder;
		result = latency_open_irtt(&context->latency, targets, target_count,
					   config->reflector_ping_interval_microseconds,
					   config->irtt_session_duration_minutes,
					   config->ping_extra_args, config->ping_prefix_string,
					   first_start_microseconds, error, sizeof(error));
	} else {
		result = latency_open(&context->latency, config->interface, targets, target_count,
				      config->reflector_ping_interval_microseconds,
				      config->ping_extra_args, config->ping_prefix_string,
				      strcmp(config->pinger_method, PINGER_METHOD_FPING_TS) == 0,
				      error, sizeof(error));
	}
	if (result != 0) {
		report_latency_degraded(context, error);
		return false;
	}

	level = context->latency_observation_failed ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO;
	outcome = context->latency_observation_failed ? "recovered" : "initialized";
	if (strcmp(config->pinger_method, PINGER_METHOD_IRTT) == 0) {
		log_message(level, "latency observation %s: targets=%zu pinger=irtt", outcome,
			    target_count);
	} else {
		log_message(level, "latency observation %s: targets=%zu interface=%s", outcome,
			    target_count, config->interface);
	}
	context->latency_observation_failed = false;
	context->next_latency_attempt_microseconds = 0U;
	context->last_pinger_restart_microseconds = timestamp_microseconds;
	return true;
}

static size_t find_active_reflector(const struct observation_context *context,
				    const struct config *config, const char *target)
{
	size_t target_count = (size_t)config->no_pingers;
	size_t index;

	for (index = 0U; index < target_count; index++)
		if (strcmp(config->reflectors[context->reflector_order[index]], target) == 0)
			return index;
	return SIZE_MAX;
}

/* Returns false when latency observation must be restarted. */
static bool process_latency_line(struct observation_context *context, const struct config *config,
				 size_t child_index, const char *line)
{
	struct latency_observation observation;
	struct latency_sample sample;
	char error[ERROR_SIZE] = { 0 };
	bool stale;
	uint64_t processing_realtime_microseconds;
	uint64_t processing_monotonic_microseconds;
	uint64_t response_monotonic_microseconds;
	enum latency_probe_result result = latency_handle_line(&context->latency, child_index, line,
							       &sample, error, sizeof(error));
	size_t reflector_index;
	bool low_load;

	if (result == LATENCY_PROBE_PENDING || result == LATENCY_PROBE_TIMEOUT)
		return true;
	if (result == LATENCY_PROBE_ERROR) {
		report_latency_degraded(context, error);
		return false;
	}

	reflector_index = find_active_reflector(context, config, sample.target);
	if (reflector_index == SIZE_MAX) {
		(void)snprintf(error, sizeof(error), "unexpected reflector=%s", sample.target);
		report_latency_degraded(context, error);
		return false;
	}

	if (!read_clock_microseconds(CLOCK_REALTIME, &processing_realtime_microseconds) ||
	    !read_clock_microseconds(CLOCK_MONOTONIC, &processing_monotonic_microseconds)) {
		if (!context->response_clock_failed) {
			log_message(LOG_LEVEL_WARNING,
				    "latency observation degraded: response clock failed: %s",
				    strerror(errno));
		}
		context->response_clock_failed = true;
		return true;
	}
	if (context->response_clock_failed) {
		log_message(LOG_LEVEL_NOTICE, "latency response clock recovered");
		context->response_clock_failed = false;
	}
	low_load = direction_has_low_load(&context->download,
					  context->controller.config.high_load_threshold_percent) &&
		   direction_has_low_load(&context->upload,
					  context->controller.config.high_load_threshold_percent);

	tracker_update(&context->latency_trackers[context->reflector_order[reflector_index]],
		       &sample, &observation);

	tracker_update_delta_ewma(
		&context->latency_trackers[context->reflector_order[reflector_index]], low_load,
		&observation);
	response_timestamp(processing_realtime_microseconds, processing_monotonic_microseconds,
			   sample.timestamp_microseconds, &response_monotonic_microseconds, &stale);
	context->last_reflector_response_microseconds = response_monotonic_microseconds;
	health_record_response(&context->reflector_health[reflector_index],
			       response_monotonic_microseconds);
	if (stale) {
		log_message(LOG_LEVEL_DEBUG,
			    "processed response from [%s] that is > 500ms old. Skipping.",
			    sample.target);
		return true;
	}

	update_controller(context, config, &observation, &sample);
	return true;
}

/* Every spawned child must be known to uloop before anything can stop it. */
static void register_pinger_processes(struct event_loop *loop)
{
	size_t index;

	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct pinger_watch *watch = &loop->pingers[index];
		pid_t process_identifier = latency_child_process(&loop->observation.latency, index);

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

void close_latency(struct event_loop *loop)
{
	size_t index;

	(void)uloop_timeout_cancel(&loop->latency_start_timer);
	register_pinger_processes(loop);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++)
		release_pinger_output(&loop->pingers[index]);
	latency_close(&loop->observation.latency);
	/* uloop reaps the children; escalate once if SIGTERM is ignored. */
	if (latency_stopping(&loop->observation.latency) && !loop->pinger_stop_timer.pending)
		(void)uloop_timeout_set(&loop->pinger_stop_timer, CHILD_STOP_TIMEOUT_MILLISECONDS);
}

/* The event loop has stopped, so pingers are stopped and reaped synchronously. */
void stop_pingers_now(struct event_loop *loop)
{
	size_t index;

	(void)uloop_timeout_cancel(&loop->latency_start_timer);
	(void)uloop_timeout_cancel(&loop->pinger_stop_timer);
	(void)uloop_timeout_cancel(&loop->latency_failure_timer);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		release_pinger_output(&loop->pingers[index]);
		(void)uloop_process_delete(&loop->pingers[index].process);
	}
	latency_stop_now(&loop->observation.latency);
}

static void handle_pinger_stop_timeout(struct uloop_timeout *timer)
{
	struct event_loop *loop =
		__extension__ container_of(timer, struct event_loop, pinger_stop_timer);

	log_message(LOG_LEVEL_WARNING, "%s did not stop within %d ms of SIGTERM; sending SIGKILL",
		    loop->config->pinger_method, CHILD_STOP_TIMEOUT_MILLISECONDS);
	latency_kill_stopping(&loop->observation.latency);
}

static void defer_latency_retry(struct event_loop *loop)
{
	uint64_t timestamp_microseconds;

	close_latency(loop);
	if (read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
		loop->observation.next_latency_attempt_microseconds =
			timestamp_microseconds +
			loop->config->interface_up_check_interval_microseconds;
	}
}

static void handle_latency_failure(struct uloop_timeout *timer)
{
	struct event_loop *loop =
		__extension__ container_of(timer, struct event_loop, latency_failure_timer);

	defer_latency_retry(loop);
}

static void handle_pinger_output(struct ustream *stream, int bytes)
{
	struct pinger_watch *watch =
		__extension__ container_of(stream, struct pinger_watch, output.stream);
	struct event_loop *loop = watch->loop;
	char error[ERROR_SIZE];

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
			(void)snprintf(error, sizeof(error), "%s output line is too long",
				       loop->config->pinger_method);
			report_latency_degraded(&loop->observation, error);
			break;
		}
		ustream_consume(stream, (int)consumed);
		if (!process_latency_line(&loop->observation, loop->config, watch->child_index,
					  line)) {
			break;
		}
	}
	/* A stream cannot be freed from its read callback; restart from the loop. */
	ustream_set_read_blocked(stream, true);
	(void)uloop_timeout_set(&loop->latency_failure_timer, 0);
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
	struct event_loop *loop = watch->loop;
	struct latency *latency = &loop->observation.latency;
	char error[ERROR_SIZE] = { 0 };

	/* Deliver output written just before the exit, then stop reading. */
	if (watch->reading) {
		(void)ustream_poll(&watch->output.stream);
		release_pinger_output(watch);
	}
	switch (latency_child_exited(latency, watch->child_index, status, error, sizeof(error))) {
	case LATENCY_PROBE_STOPPED:
		if (!latency_stopping(latency)) {
			(void)uloop_timeout_cancel(&loop->pinger_stop_timer);
			/* Like cake-autorate, a restart starts once the old pingers are gone. */
			if (loop->observation.activity.state != CONTROLLER_IDLE)
				(void)watch_latency(loop);
		}
		return;
	case LATENCY_PROBE_RESTART:
		log_message(LOG_LEVEL_DEBUG, "Restarting irtt pinger: pinger=%zu (%s)",
			    watch->child_index, error);
		(void)schedule_irtt_child_start(loop);
		return;
	default:
		report_latency_degraded(&loop->observation, error);
		defer_latency_retry(loop);
		return;
	}
}

static bool watch_started_latency_children(struct event_loop *loop)
{
	const struct latency *latency = &loop->observation.latency;
	size_t index;

	register_pinger_processes(loop);
	for (index = 0U; index < latency_child_count(latency); index++) {
		struct pinger_watch *watch = &loop->pingers[index];
		int descriptor = latency_child_descriptor(latency, index);

		if (watch->reading || descriptor < 0)
			continue;
		watch->output =
			(struct ustream_fd){ .stream = { .notify_read = handle_pinger_output,
							 .notify_state = handle_pinger_state } };
		ustream_fd_init(&watch->output, descriptor);
		watch->reading = true;
		/* ustream_fd_init() does not report a failed registration itself. */
		if (!watch->output.fd.registered) {
			char error[ERROR_SIZE];

			(void)snprintf(error, sizeof(error), "could not monitor %s output",
				       loop->config->pinger_method);
			report_latency_degraded(&loop->observation, error);
			close_latency(loop);
			return false;
		}
	}
	return true;
}

static bool schedule_irtt_child_start(struct event_loop *loop)
{
	char error[ERROR_SIZE] = { 0 };
	uint64_t timestamp_microseconds;
	uint64_t next_start_microseconds;
	uint64_t delay_microseconds;
	uint64_t delay_milliseconds;

	if (!latency_irtt_start_pending(&loop->observation.latency)) {
		(void)uloop_timeout_cancel(&loop->latency_start_timer);
		return true;
	}
	if (!read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
		(void)snprintf(error, sizeof(error), "IRTT start clock failed: %s",
			       strerror(errno));
		report_latency_degraded(&loop->observation, error);
		defer_latency_retry(loop);
		return false;
	}
	if (latency_start_irtt_children(&loop->observation.latency, timestamp_microseconds, error,
					sizeof(error)) != 0) {
		report_latency_degraded(&loop->observation, error);
		defer_latency_retry(loop);
		return false;
	}
	if (!watch_started_latency_children(loop))
		return false;
	if (!latency_irtt_start_pending(&loop->observation.latency))
		return true;

	next_start_microseconds = latency_irtt_next_start_microseconds(&loop->observation.latency);
	delay_microseconds = next_start_microseconds > timestamp_microseconds
				     ? next_start_microseconds - timestamp_microseconds
				     : 0U;
	delay_milliseconds = milliseconds_rounded_up(delay_microseconds);
	if (delay_milliseconds > (uint64_t)INT_MAX)
		delay_milliseconds = (uint64_t)INT_MAX;
	if (uloop_timeout_set(&loop->latency_start_timer, (int)delay_milliseconds) != 0) {
		(void)snprintf(error, sizeof(error), "could not schedule IRTT start: %s",
			       strerror(errno));
		report_latency_degraded(&loop->observation, error);
		defer_latency_retry(loop);
		return false;
	}
	return true;
}

static void handle_latency_start(struct uloop_timeout *timer)
{
	struct event_loop *loop =
		__extension__ container_of(timer, struct event_loop, latency_start_timer);

	(void)schedule_irtt_child_start(loop);
}

bool watch_latency(struct event_loop *loop)
{
	if (!ensure_latency_open(&loop->observation, loop->config))
		return false;
	if (strcmp(loop->config->pinger_method, PINGER_METHOD_IRTT) == 0)
		return schedule_irtt_child_start(loop);
	return watch_started_latency_children(loop);
}

/* Match cake-autorate's two-period setup grace after (re)starting pingers. */
void grant_pinger_setup_grace(struct observation_context *context, const struct config *config,
			      uint64_t timestamp_microseconds)
{
	context->last_reflector_response_microseconds = timestamp_microseconds;
	context->pinger_grace_until_microseconds =
		timestamp_microseconds + 2U * config->reflector_ping_interval_microseconds;
	reset_reflector_health(context, config, context->pinger_grace_until_microseconds);
}

void restart_latency(struct event_loop *loop, uint64_t timestamp_microseconds)
{
	close_latency(loop);
	loop->observation.next_latency_attempt_microseconds = 0U;
	reset_reflector_health(&loop->observation, loop->config, timestamp_microseconds);
	loop->observation.last_pinger_restart_microseconds = timestamp_microseconds;
	(void)watch_latency(loop);
}

/* Each slot and timer is bound to its handler before uloop starts. */
void prepare_pingers(struct event_loop *loop)
{
	size_t index;

	loop->latency_start_timer.cb = handle_latency_start;
	loop->pinger_stop_timer.cb = handle_pinger_stop_timeout;
	loop->latency_failure_timer.cb = handle_latency_failure;
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		loop->pingers[index].process.cb = handle_pinger_exit;
		loop->pingers[index].loop = loop;
		loop->pingers[index].child_index = index;
	}
	latency_init(&loop->observation.latency);
}
