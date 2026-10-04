#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/pinger.h"
#include "common/constants.h"
#include "common/error.h"
#include "common/helpers.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

static int
spawn_irtt_child(struct latency *latency, size_t child_index, char *error, size_t error_size)
{
	struct latency_child *child = &latency->children[child_index];
	struct pinger_command command;
	char endpoint[LATENCY_TARGET_SIZE + 3U];
	char interval[32];
	char duration[32];
	size_t index;
	int ret;

	if (pinger_command_init(
		    &command,
		    latency->ping_prefix_string,
		    latency->ping_extra_args,
		    7U,
		    PINGER_METHOD_IRTT,
		    error,
		    error_size
	    ) != 0)
		return -1;

	(void)snprintf(
		interval,
		sizeof(interval),
		"%" PRIu64 ".%06" PRIu64 "s",
		latency->reflector_ping_interval_microseconds / SECOND,
		latency->reflector_ping_interval_microseconds % SECOND
	);
	(void)snprintf(
		duration,
		sizeof(duration),
		"%" PRIu64 "m",
		latency->irtt_session_duration_minutes
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

	ret = start_child(child, command.argv, PINGER_METHOD_IRTT, error, error_size);
	pinger_command_free(&command);
	return ret;
}

int latency_open_irtt(
	struct latency *latency,
	const char *const *targets,
	size_t target_count,
	uint64_t reflector_ping_interval_microseconds,
	uint64_t session_duration_minutes,
	const char *extra_arguments,
	const char *prefix,
	uint64_t first_start_microseconds,
	char *error,
	size_t error_size
)
{
	wordexp_t words = { 0 };
	uint64_t child_start_spacing_microseconds;
	size_t index;

	if (targets == NULL || target_count == 0U || target_count > CONFIG_MAX_REFLECTORS ||
	    reflector_ping_interval_microseconds == 0U ||
	    reflector_ping_interval_microseconds / target_count < MILLISECOND ||
	    session_duration_minutes == 0U || extra_arguments == NULL || prefix == NULL) {
		return error_set(error, error_size, "invalid irtt session configuration");
	}
	if (expand_words(extra_arguments, false, OPTION_PING_EXTRA_ARGS, &words, error, error_size) !=
	    0) {
		return -1;
	}
	wordfree(&words);
	words = (wordexp_t){ 0 };
	if (expand_words(prefix, true, OPTION_PING_PREFIX_STRING, &words, error, error_size) != 0)
		return -1;
	wordfree(&words);
	if (validate_targets(targets, target_count, error, error_size) != 0)
		return -1;

	child_start_spacing_microseconds = reflector_ping_interval_microseconds / target_count;
	for (index = 0U; index < target_count; index++) {
		latency->children[index].target = targets[index];
		latency->children[index].next_start_microseconds =
			first_start_microseconds + index * child_start_spacing_microseconds;
	}
	latency->ops = &irtt_ops;
	latency->active = true;
	latency->irtt_session_duration_minutes = session_duration_minutes;
	latency->reflector_ping_interval_microseconds = reflector_ping_interval_microseconds;
	latency->ping_extra_args = extra_arguments;
	latency->ping_prefix_string = prefix;
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

	if (!latency->active || latency->ops != &irtt_ops)
		return error_set(error, error_size, "irtt session is not active");
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

	if (!parse_irtt_line(line, child->target, 0U, sample))
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
	runtime_microseconds = timestamp_microseconds >= child->started_microseconds ?
				       timestamp_microseconds - child->started_microseconds :
				       0U;
	child->next_start_microseconds = timestamp_microseconds;
	if (runtime_microseconds < IRTT_FAST_EXIT_THRESHOLD_MICROSECONDS)
		child->next_start_microseconds += IRTT_FAST_EXIT_RETRY_MICROSECONDS;
	return LATENCY_PROBE_RESTART;
}

const struct pinger_ops irtt_ops = {
	.name = PINGER_METHOD_IRTT,
	.parse = irtt_parse,
	.exited = irtt_exited,
};
