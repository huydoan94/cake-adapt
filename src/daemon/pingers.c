#define _GNU_SOURCE

#include "daemon/state.h"

#include "common/clock.h"
#include "common/convert.h"
#include "latency/response.h"
#include "logging/log.h"
#include "constants/latency.h"
#include "constants/options.h"
#include "constants/states.h"
#include "constants/text.h"

#include <libubox/utils.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void pingers_report_degraded(struct daemon *daemon, const char *error)
{
	struct daemon_pingers *pingers = &daemon->pingers;

	/* Not daemon_degraded(): its message buffer could cut a long error short. */
	if (!pingers->observation_failed)
		log_message(LOG_LEVEL_WARNING, "latency observation degraded: %s", error);
	pingers->observation_failed = true;
}

void pingers_report_failure(struct daemon *daemon, const char *format, ...)
{
	char error[ERROR_SIZE];
	va_list arguments;

	va_start(arguments, format);
	(void)vsnprintf(error, sizeof(error), format, arguments);
	va_end(arguments);
	pingers_report_degraded(daemon, error);
}

/* Like interface checks, a failed pinger start is retried after interface_up_check_interval_s. */
static void schedule_retry(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_pingers *pingers = &daemon->pingers;

	pingers->next_attempt_us = timestamp_us + daemon->config->interface_up_check_interval_us;
}

static bool ensure_latency_open(struct daemon *daemon)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	struct latency *latency = &pingers->latency;
	const struct config *config = daemon->config;
	const char *targets[CONFIG_MAX_REFLECTORS];
	char error[ERROR_SIZE] = { 0 };
	size_t target_count = (size_t)config->no_pingers;
	uint64_t timestamp_us;
	enum log_level level;
	const char *outcome;

	if (!links_ready(daemon))
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
	schedule_retry(daemon, timestamp_us);
	reflectors_active(daemon, targets);
	if (latency_open(latency, targets, target_count, timestamp_us, error, sizeof(error)) != 0) {
		pingers_report_degraded(daemon, error);
		return false;
	}

	level = pingers->observation_failed ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO;
	outcome = pingers->observation_failed ? STATE_RECOVERED : STATE_INITIALIZED;
	if (daemon->pinger_method == PINGER_METHOD_IRTT) {
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

bool pingers_process_sample(
	struct daemon *daemon,
	const struct latency_sample *sample,
	const uint64_t *captured_response_us
)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	struct latency_observation observation;
	uint64_t processing_realtime_us = 0U;
	uint64_t processing_monotonic_us;
	uint64_t response_us;
	bool stale;
	size_t slot;

	slot = reflectors_find(daemon, sample->target);
	if (slot == SIZE_MAX) {
		pingers_report_failure(daemon, "unexpected reflector=%s", sample->target);
		return false;
	}

	if (!read_clock_us(CLOCK_MONOTONIC, &processing_monotonic_us) ||
	    (captured_response_us == NULL &&
	     !read_clock_us(CLOCK_REALTIME, &processing_realtime_us))) {
		daemon_degraded(
			&pingers->response_clock_failed,
			"latency observation degraded: response clock failed: %s",
			strerror(errno)
		);
		return true;
	}
	daemon_recovered(&pingers->response_clock_failed, "latency response clock recovered");
	/* Native probes bring the kernel's receive time; a pinger's line its own timestamp. */
	if (captured_response_us != NULL) {
		response_us = *captured_response_us;
		stale = response_stale(processing_monotonic_us, response_us);
	} else {
		response_us = response_monotonic_us(
			processing_realtime_us,
			processing_monotonic_us,
			sample->timestamp_us
		);
		stale = response_stale(processing_realtime_us, sample->timestamp_us);
	}
	reflectors_record(daemon, slot, sample, response_us, &observation);
	pingers->last_response_us = response_us;
	if (stale) {
		log_message(
			LOG_LEVEL_DEBUG,
			"processed response from [%s] that is > 500ms old. Skipping.",
			sample->target
		);
		return true;
	}

	shaper_update(daemon, &observation, sample);
	return true;
}

bool pingers_process_line(struct daemon *daemon, size_t child_index, const char *line)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	struct latency_sample sample;
	char error[ERROR_SIZE] = { 0 };
	enum latency_probe_result result = latency_handle_line(
		&pingers->latency,
		child_index,
		line,
		&sample,
		error,
		sizeof(error)
	);

	if (result == LATENCY_PROBE_PENDING || result == LATENCY_PROBE_TIMEOUT)
		return true;
	if (result == LATENCY_PROBE_ERROR) {
		pingers_report_degraded(daemon, error);
		return false;
	}
	return pingers_process_sample(daemon, &sample, NULL);
}

void pingers_close(struct daemon *daemon)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	struct uloop_timeout *stop_timer = &pingers->stop_timer;
	size_t index;

	(void)uloop_timeout_cancel(&pingers->start_timer);
	native_pinger_unwatch(pingers);
	child_pingers_register(daemon);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++)
		child_pingers_release_output(&pingers->watches[index]);
	latency_close(&pingers->latency);
	/* uloop reaps the children; escalate once if SIGTERM is ignored. */
	if (latency_stopping(&pingers->latency) && !stop_timer->pending)
		(void)uloop_timeout_set(stop_timer, us_to_timeout_ms(CHILD_STOP_TIMEOUT_US));
}

/* The event loop has stopped, so pingers are stopped and reaped synchronously. */
void pingers_stop_now(struct daemon *daemon)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	size_t index;

	(void)uloop_timeout_cancel(&pingers->start_timer);
	native_pinger_unwatch(pingers);
	(void)uloop_timeout_cancel(&pingers->stop_timer);
	(void)uloop_timeout_cancel(&pingers->failure_timer);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct pinger_child_watch *watch = &pingers->watches[index];

		child_pingers_release_output(watch);
		(void)uloop_process_delete(&watch->process);
	}
	latency_stop_now(&pingers->latency);
}

void pingers_defer_retry(struct daemon *daemon)
{
	uint64_t timestamp_us;

	pingers_close(daemon);
	if (read_clock_us(CLOCK_MONOTONIC, &timestamp_us))
		schedule_retry(daemon, timestamp_us);
}

static void handle_latency_failure(struct uloop_timeout *timer)
{
	struct daemon *daemon =
		__extension__ container_of(timer, struct daemon, pingers.failure_timer);

	pingers_defer_retry(daemon);
}

bool pingers_watch(struct daemon *daemon)
{
	if (!ensure_latency_open(daemon))
		return false;
	if (pinger_is_native(daemon->pinger_method))
		return native_pinger_watch(daemon);
	if (daemon->pinger_method == PINGER_METHOD_IRTT)
		return child_pingers_start_irtt(daemon);
	return child_pingers_watch(daemon);
}

/* Match cake-autorate's two-period setup grace after (re)starting pingers. */
static void grant_grace(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	const struct config *config = daemon->config;

	pingers->last_response_us = timestamp_us;
	pingers->grace_until_us = timestamp_us + 2U * config->reflector_ping_interval_us;
	reflectors_reset_health(daemon, pingers->grace_until_us);
}

void pingers_suspend(struct daemon *daemon)
{
	struct daemon_pingers *pingers = &daemon->pingers;

	pingers_close(daemon);
	pingers->suspended = true;
}

/* Pingers stopped while CAKE was missing restart like after IDLE. */
void pingers_unsuspend(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_pingers *pingers = &daemon->pingers;

	if (!pingers->suspended)
		return;
	grant_grace(daemon, timestamp_us);
	pingers->suspended = false;
}

void pingers_resume(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_pingers *pingers = &daemon->pingers;

	grant_grace(daemon, timestamp_us);
	pingers->next_attempt_us = 0U;
	(void)pingers_watch(daemon);
}

void pingers_reopen(struct daemon *daemon)
{
	struct daemon_pingers *pingers = &daemon->pingers;

	pingers_close(daemon);
	pingers->next_attempt_us = 0U;
	(void)pingers_watch(daemon);
}

void pingers_restart(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_pingers *pingers = &daemon->pingers;

	reflectors_reset_health(daemon, timestamp_us);
	pingers->last_restart_us = timestamp_us;
	pingers_reopen(daemon);
}

/* Each slot and timer is bound to its handler before uloop starts. */
void pingers_prepare(struct daemon *daemon, uint64_t start_us)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	const struct config *config = daemon->config;
	const struct latency_settings settings = {
		.method = daemon->pinger_method,
		.interface = config->interface,
		.extra_arguments = config->ping_extra_args,
		.prefix = config->ping_prefix_string,
		.reflector_ping_interval_us = config->reflector_ping_interval_us,
		.irtt_session_duration_us = config->irtt_session_duration_us,
		.slot_origin_us = start_us,
	};

	pingers->last_response_us = start_us;
	pingers->last_restart_us = start_us;
	pingers->failure_timer = (struct uloop_timeout){ .cb = handle_latency_failure };
	native_pinger_prepare(daemon);
	child_pingers_prepare(daemon);
	latency_init(&pingers->latency, &settings);
}
