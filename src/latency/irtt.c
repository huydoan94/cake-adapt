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

static int spawn_irtt_child(struct latency *latency, size_t child_index, char *error,
			    size_t error_size)
{
	struct latency_child *child = &latency->children[child_index];
	char interval[32];
	char duration[32];
	char endpoint[LATENCY_TARGET_SIZE + 3U];
	char **arguments = NULL;
	wordexp_t extra_words = { 0 };
	wordexp_t prefix_words = { 0 };
	size_t cursor = 0U;
	size_t index;
	int result = -1;

	if (expand_words(latency->ping_extra_args, false, OPTION_PING_EXTRA_ARGS, &extra_words,
			 error, error_size) != 0 ||
	    expand_words(latency->ping_prefix_string, true, OPTION_PING_PREFIX_STRING,
			 &prefix_words, error, error_size) != 0) {
		goto done;
	}

	(void)snprintf(interval, sizeof(interval), "%" PRIu64 ".%06" PRIu64 "s",
		       latency->reflector_ping_interval_microseconds / SECOND,
		       latency->reflector_ping_interval_microseconds % SECOND);
	(void)snprintf(duration, sizeof(duration), "%" PRIu64 "m",
		       latency->irtt_session_duration_minutes);
	if (strchr(child->target, ':') == NULL)
		(void)strcpy(endpoint, child->target);
	else
		(void)snprintf(endpoint, sizeof(endpoint), "[%s]", child->target);

	arguments = calloc(prefix_words.we_wordc + extra_words.we_wordc + 8U, sizeof(*arguments));
	if (arguments == NULL) {
		error_set(error, error_size, "could not allocate irtt arguments: %s",
			  strerror(errno));
		goto done;
	}
	for (index = 0U; index < prefix_words.we_wordc; index++)
		arguments[cursor++] = prefix_words.we_wordv[index];
	arguments[cursor++] = (char *)IRTT_PATH;
	arguments[cursor++] = (char *)IRTT_CLIENT;
	for (index = 0U; index < extra_words.we_wordc; index++)
		arguments[cursor++] = extra_words.we_wordv[index];
	arguments[cursor++] = (char *)IRTT_INTERVAL;
	arguments[cursor++] = interval;
	arguments[cursor++] = (char *)IRTT_DURATION;
	arguments[cursor++] = duration;
	arguments[cursor++] = endpoint;

	result = start_child(child, arguments, PINGER_METHOD_IRTT, error, error_size);

done:
	free(arguments);
	wordfree(&prefix_words);
	wordfree(&extra_words);
	return result;
}

int latency_open_irtt(struct latency *latency, const char *const *targets, size_t target_count,
		      uint64_t reflector_ping_interval_microseconds,
		      uint64_t session_duration_minutes, const char *extra_arguments,
		      const char *prefix, uint64_t first_start_microseconds, char *error,
		      size_t error_size)
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
	if (expand_words(extra_arguments, false, OPTION_PING_EXTRA_ARGS, &words, error,
			 error_size) != 0) {
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
	latency->backend = LATENCY_BACKEND_IRTT;
	latency->active = true;
	latency->irtt_session_duration_minutes = session_duration_minutes;
	latency->reflector_ping_interval_microseconds = reflector_ping_interval_microseconds;
	latency->ping_extra_args = extra_arguments;
	latency->ping_prefix_string = prefix;
	latency->child_count = target_count;
	return 0;
}

int latency_start_irtt_children(struct latency *latency, uint64_t timestamp_microseconds,
				char *error, size_t error_size)
{
	size_t index;

	if (!latency->active || latency->backend != LATENCY_BACKEND_IRTT)
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

	if (!latency->active || latency->backend != LATENCY_BACKEND_IRTT)
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

enum latency_probe_result schedule_irtt_restart(struct latency_child *child, char *error,
						size_t error_size)
{
	uint64_t timestamp_microseconds;
	uint64_t runtime_microseconds;

	if (!read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
		error_set(error, error_size, "could not schedule irtt restart: %s",
			  strerror(errno));
		return LATENCY_PROBE_ERROR;
	}
	runtime_microseconds = timestamp_microseconds >= child->started_microseconds
				       ? timestamp_microseconds - child->started_microseconds
				       : 0U;
	child->next_start_microseconds = timestamp_microseconds;
	if (runtime_microseconds < IRTT_FAST_EXIT_THRESHOLD_MICROSECONDS)
		child->next_start_microseconds += IRTT_FAST_EXIT_RETRY_MICROSECONDS;
	return LATENCY_PROBE_RESTART;
}
