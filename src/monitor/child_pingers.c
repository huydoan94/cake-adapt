#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"
#include "config/defaults.h"
#include "constants/latency.h"
#include "constants/options.h"
#include "constants/text.h"

#include <libubox/utils.h>

#include <errno.h>
#include <string.h>

void child_pingers_register(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	size_t index;

	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct uloop_process *process = &pingers->watches[index].process;
		pid_t process_identifier = latency_child_process(&pingers->latency, index);

		if (process_identifier > 0 && !process->pending) {
			process->pid = process_identifier;
			(void)uloop_process_add(process);
		}
	}
}

void child_pingers_release_output(struct pinger_watch *watch)
{
	struct ustream_fd *output = &watch->output;

	if (watch->reading) {
		ustream_free(&output->stream);
		watch->reading = false;
	}
}

static void handle_pinger_stop_timeout(struct uloop_timeout *timer)
{
	struct monitor *monitor =
		__extension__ container_of(timer, struct monitor, pingers.stop_timer);
	struct monitor_pingers *pingers = &monitor->pingers;

	log_message(
		LOG_LEVEL_WARNING,
		"%s did not stop within %u ms of SIGTERM; sending SIGKILL",
		monitor->config->pinger_method,
		us_to_ms(CHILD_STOP_TIMEOUT_US)
	);
	latency_kill_stopping(&pingers->latency);
}

static void handle_pinger_output(struct ustream *stream, int bytes)
{
	struct pinger_watch *watch =
		__extension__ container_of(stream, struct pinger_watch, output.stream);
	struct monitor *monitor = watch->monitor;
	struct monitor_pingers *pingers = &monitor->pingers;
	/* A stream cannot be freed from its read callback; restart from the loop. */
	const bool stop_reading = true;

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
			pingers_report_failure(
				monitor,
				"%s output line is too long",
				monitor->config->pinger_method
			);
			break;
		}
		ustream_consume(stream, (int)consumed);
		if (!pingers_process_line(monitor, watch->child_index, line))
			break;
	}
	ustream_set_read_blocked(stream, stop_reading);
	(void)uloop_timeout_set(&pingers->failure_timer, 0);
}

/* End of output or a descriptor error; the exit callback reports the cause. */
static void handle_pinger_state(struct ustream *stream)
{
	struct pinger_watch *watch =
		__extension__ container_of(stream, struct pinger_watch, output.stream);

	child_pingers_release_output(watch);
}

static void handle_pinger_exit(struct uloop_process *process, int status)
{
	struct pinger_watch *watch =
		__extension__ container_of(process, struct pinger_watch, process);
	struct monitor *monitor = watch->monitor;
	struct monitor_pingers *pingers = &monitor->pingers;
	struct latency *latency = &pingers->latency;
	struct ustream_fd *output = &watch->output;
	const struct controller_activity *activity = &monitor->activity;
	char error[ERROR_SIZE] = { 0 };

	/* Deliver output written just before the exit, then stop reading. */
	if (watch->reading) {
		(void)ustream_poll(&output->stream);
		child_pingers_release_output(watch);
	}
	switch (latency_child_exited(latency, watch->child_index, status, error, sizeof(error))) {
	case LATENCY_PROBE_STOPPED:
		if (!latency_stopping(latency)) {
			(void)uloop_timeout_cancel(&pingers->stop_timer);
			/* Like cake-autorate, a restart starts once the old pingers are gone. */
			if (activity->state != CONTROLLER_IDLE)
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
		(void)child_pingers_start_irtt(monitor);
		return;
	default:
		pingers_report_degraded(monitor, error);
		pingers_defer_retry(monitor);
		return;
	}
}

bool child_pingers_watch(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	const struct latency *latency = &pingers->latency;
	size_t index;

	child_pingers_register(monitor);
	for (index = 0U; index < latency_child_count(latency); index++) {
		struct pinger_watch *watch = &pingers->watches[index];
		struct ustream_fd *output = &watch->output;
		const struct uloop_fd *reader = &output->fd;
		int descriptor = latency_child_descriptor(latency, index);

		if (watch->reading || descriptor < 0)
			continue;
		*output = (struct ustream_fd){ .stream = {
						       .notify_read = handle_pinger_output,
						       .notify_state = handle_pinger_state,
					       } };
		ustream_fd_init(output, descriptor);
		watch->reading = true;
		/* ustream_fd_init() does not report a failed registration itself. */
		if (!reader->registered) {
			pingers_report_failure(
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

bool child_pingers_start_irtt(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	struct latency *latency = &pingers->latency;
	char error[ERROR_SIZE] = { 0 };
	uint64_t timestamp_us;
	uint64_t next_start_us;

	if (!latency_irtt_start_pending(latency)) {
		(void)uloop_timeout_cancel(&pingers->start_timer);
		return true;
	}
	if (!read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		pingers_report_failure(monitor, "IRTT start clock failed: %s", strerror(errno));
		pingers_defer_retry(monitor);
		return false;
	}
	if (latency_start_irtt_children(latency, timestamp_us, error, sizeof(error)) != 0) {
		pingers_report_degraded(monitor, error);
		pingers_defer_retry(monitor);
		return false;
	}
	if (!child_pingers_watch(monitor))
		return false;
	if (!latency_irtt_start_pending(latency))
		return true;

	next_start_us = latency_irtt_next_start_us(latency);
	if (uloop_timeout_set(
		    &pingers->start_timer,
		    us_to_timeout_ms(saturating_sub(next_start_us, timestamp_us))
	    ) != 0) {
		pingers_report_failure(
			monitor,
			"could not schedule IRTT start: %s",
			strerror(errno)
		);
		pingers_defer_retry(monitor);
		return false;
	}
	return true;
}

static void handle_latency_start(struct uloop_timeout *timer)
{
	struct monitor *monitor =
		__extension__ container_of(timer, struct monitor, pingers.start_timer);

	(void)child_pingers_start_irtt(monitor);
}

void child_pingers_prepare(struct monitor *monitor)
{
	struct monitor_pingers *pingers = &monitor->pingers;
	size_t index;

	pingers->start_timer = (struct uloop_timeout){ .cb = handle_latency_start };
	pingers->stop_timer = (struct uloop_timeout){ .cb = handle_pinger_stop_timeout };
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct pinger_watch *watch = &pingers->watches[index];

		watch->process = (struct uloop_process){ .cb = handle_pinger_exit };
		watch->monitor = monitor;
		watch->child_index = index;
	}
}
