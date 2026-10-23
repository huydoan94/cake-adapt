#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/pinger.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "constants/latency.h"
#include "constants/units.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* irtt's "<seconds>.<microseconds>s" duration argument. */
static void format_duration(char text[PINGER_ARGUMENT_SIZE], uint64_t duration_us)
{
	(void)snprintf(
		text,
		PINGER_ARGUMENT_SIZE,
		"%" PRIu64 ".%06" PRIu64 "s",
		duration_us / MICROSECONDS_PER_SECOND,
		duration_us % MICROSECONDS_PER_SECOND
	);
}

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

	format_duration(interval, settings->reflector_ping_interval_us);
	format_duration(duration, settings->irtt_session_duration_us);
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

/* Sessions start one per target, spread over the ping slot after timestamp_us. */
static int irtt_open(
	struct latency *latency,
	const char *const *targets,
	size_t target_count,
	uint64_t timestamp_us,
	char *error,
	size_t error_size
)
{
	const struct latency_settings *settings = &latency->settings;
	uint64_t interval_us = settings->reflector_ping_interval_us;
	uint64_t elapsed_us = timestamp_us - settings->slot_origin_us;
	uint64_t first_start_us = timestamp_us + interval_us - elapsed_us % interval_us;
	uint64_t spacing_us = rounded_divide(interval_us, target_count);
	struct pinger_command command;
	size_t index;

	/* Each child expands the options again when it starts; check them now. */
	if (pinger_command_init(&command, latency, 0U, error, error_size) != 0)
		return -1;
	pinger_command_free(&command);

	for (index = 0U; index < target_count; index++) {
		struct latency_child *child = &latency->children[index];

		child->target = targets[index];
		child->next_start_us = first_start_us + index * spacing_us;
	}
	latency->active = true;
	latency->child_count = target_count;
	return 0;
}

int latency_start_irtt_children(
	struct latency *latency,
	uint64_t timestamp_us,
	char *error,
	size_t error_size
)
{
	size_t index;

	for (index = 0U; index < latency->child_count; index++) {
		struct latency_child *child = &latency->children[index];

		if (child->output_descriptor >= 0 || timestamp_us < child->next_start_us)
			continue;
		if (spawn_irtt_child(latency, index, error, error_size) != 0) {
			latency_close(latency);
			return -1;
		}
		child->started_us = timestamp_us;
	}
	return 0;
}

bool latency_irtt_start_pending(const struct latency *latency)
{
	const struct latency_settings *settings = &latency->settings;
	size_t index;

	if (!latency->active || settings->method != PINGER_IRTT)
		return false;
	for (index = 0U; index < latency->child_count; index++) {
		const struct latency_child *child = &latency->children[index];

		if (child->output_descriptor < 0)
			return true;
	}
	return false;
}

uint64_t latency_irtt_next_start_us(const struct latency *latency)
{
	uint64_t next_us = UINT64_MAX;
	size_t index;

	for (index = 0U; index < latency->child_count; index++) {
		const struct latency_child *child = &latency->children[index];

		if (child->output_descriptor < 0 && child->next_start_us < next_us)
			next_us = child->next_start_us;
	}
	return next_us;
}

static enum latency_probe_result irtt_parse(
	const struct latency_child *child,
	const char *line,
	struct latency_sample *sample,
	char *error,
	size_t error_size
)
{
	char *timestamp_text = sample->timestamp_text;
	uint64_t timestamp_us;

	if (!parse_irtt_line(line, child->target, sample))
		return LATENCY_PROBE_PENDING;
	if (!read_clock_us(CLOCK_REALTIME, &timestamp_us)) {
		error_set(error, error_size, "could not timestamp irtt output: %s", strerror(errno));
		return LATENCY_PROBE_ERROR;
	}
	sample->timestamp_us = timestamp_us;
	(void)snprintf(timestamp_text, LATENCY_TIMESTAMP_TEXT_SIZE, "%" PRIu64, timestamp_us);
	return LATENCY_PROBE_SUCCESS;
}

/* IRTT sessions end by design: restart, delayed after a fast exit. */
static enum latency_probe_result
irtt_exited(struct latency_child *child, char *error, size_t error_size)
{
	uint64_t timestamp_us;
	uint64_t runtime_us;

	if (!read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		error_set(error, error_size, "could not schedule irtt restart: %s", strerror(errno));
		return LATENCY_PROBE_ERROR;
	}
	runtime_us = saturating_sub(timestamp_us, child->started_us);
	child->next_start_us = timestamp_us;
	if (runtime_us < IRTT_FAST_EXIT_THRESHOLD_US)
		child->next_start_us += IRTT_FAST_EXIT_RETRY_US;
	return LATENCY_PROBE_RESTART;
}

const struct pinger_ops irtt_ops = {
	.name = PINGER_METHOD_IRTT,
	.open = irtt_open,
	.parse = irtt_parse,
	.exited = irtt_exited,
};
