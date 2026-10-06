#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/pinger.h"
#include "common/constants.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int
spawn_irtt_child(struct latency *latency, size_t child_index, char *error, size_t error_size)
{
	const struct latency_settings *settings = &latency->settings;
	const struct latency_child *child = &latency->children[child_index];
	struct pinger_command command;
	char endpoint[LATENCY_TARGET_SIZE + 3U];
	char interval[PINGER_ARGUMENT_SIZE];
	char duration[PINGER_ARGUMENT_SIZE];
	size_t index;
	int ret;

	if (pinger_command_init(&command, latency, 7U, error, error_size) != 0)
		return -1;

	(void)snprintf(
		interval,
		sizeof(interval),
		"%" PRIu64 ".%06" PRIu64 "s",
		settings->reflector_ping_interval_microseconds / SECOND,
		settings->reflector_ping_interval_microseconds % SECOND
	);
	(void)snprintf(
		duration,
		sizeof(duration),
		"%" PRIu64 "m",
		settings->irtt_session_duration_minutes
	);
	if (strchr(child->target, ':') == NULL)
		(void)strcpy(endpoint, child->target);
	else
		(void)snprintf(endpoint, sizeof(endpoint), "[%s]", child->target);

	pinger_command_add(&command, IRTT_PATH);
	pinger_command_add(&command, IRTT_CLIENT);
	for (index = 0U; index < command.extra.we_wordc; index++)
		pinger_command_add(&command, command.extra.we_wordv[index]);
	pinger_command_add(&command, IRTT_INTERVAL);
	pinger_command_add(&command, interval);
	pinger_command_add(&command, IRTT_DURATION);
	pinger_command_add(&command, duration);
	pinger_command_add(&command, endpoint);

	ret = start_child(latency, child_index, command.argv, error, error_size);
	pinger_command_free(&command);
	return ret;
}

/* Sessions start one per target, spread over the ping slot after timestamp_microseconds. */
static int irtt_open(
	struct latency *latency,
	const char *const *targets,
	size_t target_count,
	uint64_t timestamp_microseconds,
	char *error,
	size_t error_size
)
{
	const struct latency_settings *settings = &latency->settings;
	uint64_t interval = settings->reflector_ping_interval_microseconds;
	uint64_t elapsed = timestamp_microseconds - settings->slot_origin_microseconds;
	uint64_t first_start_microseconds = timestamp_microseconds + interval - elapsed % interval;
	uint64_t spacing = interval / target_count;
	struct pinger_command command;
	size_t index;

	/* Each child expands the options again when it starts; check them now. */
	if (pinger_command_init(&command, latency, 0U, error, error_size) != 0)
		return -1;
	pinger_command_free(&command);

	for (index = 0U; index < target_count; index++) {
		struct latency_child *child = &latency->children[index];

		child->target = targets[index];
		child->next_start_microseconds = first_start_microseconds + index * spacing;
	}
	latency->active = true;
	latency->child_count = target_count;
	return 0;
}

int latency_start_irtt_children(
	struct latency *latency,
	uint64_t timestamp_microseconds,
	char *error,
	size_t error_size
)
{
	size_t index;

	for (index = 0U; index < latency->child_count; index++) {
		struct latency_child *child = &latency->children[index];

		if (child->output_descriptor >= 0 ||
		    timestamp_microseconds < child->next_start_microseconds) {
			continue;
		}
		if (spawn_irtt_child(latency, index, error, error_size) != 0) {
			latency_close(latency);
			return -1;
		}
		child->started_microseconds = timestamp_microseconds;
	}
	return 0;
}

bool latency_irtt_start_pending(const struct latency *latency)
{
	size_t index;

	if (!latency->active || latency->ops != &irtt_ops)
		return false;
	for (index = 0U; index < latency->child_count; index++)
		if (latency->children[index].output_descriptor < 0)
			return true;
	return false;
}

uint64_t latency_irtt_next_start_microseconds(const struct latency *latency)
{
	uint64_t next = UINT64_MAX;
	size_t index;

	for (index = 0U; index < latency->child_count; index++) {
		const struct latency_child *child = &latency->children[index];

		if (child->output_descriptor < 0 && child->next_start_microseconds < next)
			next = child->next_start_microseconds;
	}
	return next;
}

static enum latency_probe_result irtt_parse(
	const struct latency_child *child,
	const char *line,
	struct latency_sample *sample,
	char *error,
	size_t error_size
)
{
	uint64_t timestamp_microseconds;

	if (!parse_irtt_line(line, child->target, sample))
		return LATENCY_PROBE_PENDING;
	if (!read_clock_microseconds(CLOCK_REALTIME, &timestamp_microseconds)) {
		error_set(error, error_size, "could not timestamp irtt output: %s", strerror(errno));
		return LATENCY_PROBE_ERROR;
	}
	sample->timestamp_microseconds = timestamp_microseconds;
	(void)snprintf(
		sample->timestamp_text,
		sizeof(sample->timestamp_text),
		"%" PRIu64,
		timestamp_microseconds
	);
	return LATENCY_PROBE_SUCCESS;
}

/* IRTT sessions end by design: restart, delayed after a fast exit. */
static enum latency_probe_result
irtt_exited(struct latency_child *child, char *error, size_t error_size)
{
	uint64_t timestamp_microseconds;
	uint64_t runtime_microseconds;

	if (!read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
		error_set(error, error_size, "could not schedule irtt restart: %s", strerror(errno));
		return LATENCY_PROBE_ERROR;
	}
	runtime_microseconds = saturating_sub(timestamp_microseconds, child->started_microseconds);
	child->next_start_microseconds = timestamp_microseconds;
	if (runtime_microseconds < IRTT_FAST_EXIT_THRESHOLD_MICROSECONDS)
		child->next_start_microseconds += IRTT_FAST_EXIT_RETRY_MICROSECONDS;
	return LATENCY_PROBE_RESTART;
}

const struct pinger_ops irtt_ops = {
	.name = PINGER_METHOD_IRTT,
	.open = irtt_open,
	.parse = irtt_parse,
	.exited = irtt_exited,
};
