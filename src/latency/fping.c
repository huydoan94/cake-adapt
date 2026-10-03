#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/pinger.h"
#include "common/constants.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "config/defaults.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wordexp.h>

static enum latency_probe_result probe_result(enum latency_fping_line_result parsed,
					      const char *line, char *error, size_t error_size)
{
	if (parsed == LATENCY_FPING_LINE_INVALID) {
		error_set(error, error_size, "unexpected fping output: %.160s", line);
		return LATENCY_PROBE_ERROR;
	}
	return parsed == LATENCY_FPING_LINE_SAMPLE ? LATENCY_PROBE_SUCCESS : LATENCY_PROBE_TIMEOUT;
}

static enum latency_probe_result fping_parse(const struct latency_child *child, const char *line,
					     struct latency_sample *sample, char *error,
					     size_t error_size)
{
	(void)child;
	return probe_result(parse_fping_line(line, sample), line, error, error_size);
}

static enum latency_probe_result fping_ts_parse(const struct latency_child *child, const char *line,
						struct latency_sample *sample, char *error,
						size_t error_size)
{
	(void)child;
	return probe_result(parse_fping_timestamp_line(line, sample), line, error, error_size);
}

/* fping runs until stopped, so any exit is a failure. */
static enum latency_probe_result fping_exited(struct latency_child *child, char *error,
					      size_t error_size)
{
	(void)child;
	(void)error;
	(void)error_size;
	return LATENCY_PROBE_ERROR;
}

const struct pinger_ops fping_ops = {
	.name = PINGER_METHOD_FPING,
	.parse = fping_parse,
	.exited = fping_exited,
};

const struct pinger_ops fping_ts_ops = {
	.name = PINGER_METHOD_FPING,
	.parse = fping_ts_parse,
	.exited = fping_exited,
};

/* -I, --iface or --iface=NAME: the user chose the interface. */
static bool selects_interface(const char *word)
{
	return strncmp(word, FPING_INTERFACE_SHORT, strlen(FPING_INTERFACE_SHORT)) == 0 ||
	       strcmp(word, FPING_INTERFACE_LONG) == 0 ||
	       strncmp(word, FPING_INTERFACE_LONG_PREFIX, strlen(FPING_INTERFACE_LONG_PREFIX)) == 0;
}

int latency_open(struct latency *latency, const char *interface, const char *const *targets,
		 size_t target_count, uint64_t reflector_ping_interval_microseconds,
		 const char *extra_arguments, const char *prefix, bool icmp_timestamps, char *error,
		 size_t error_size)
{
	struct pinger_command command;
	char period[32];
	char response_interval[32];
	bool interface_selected = false;
	size_t index;
	int ret = -1;

	if (interface == NULL || interface[0] == '\0')
		return error_set(error, error_size, "fping interface is empty");
	if (targets == NULL || target_count == 0U || extra_arguments == NULL || prefix == NULL)
		return error_set(error, error_size, "fping requires at least one target");
	if (target_count > CONFIG_MAX_REFLECTORS)
		return error_set(error, error_size, "fping supports at most %u targets",
				 CONFIG_MAX_REFLECTORS);
	if (reflector_ping_interval_microseconds / target_count < MICROSECONDS_PER_MILLISECOND)
		return error_set(error, error_size,
				 "reflector ping interval must provide at least 1 ms per target");
	if (validate_targets(targets, target_count, error, error_size) != 0)
		return -1;

	(void)snprintf(
		period, sizeof(period), "%" PRIu64,
		rounded_divide(reflector_ping_interval_microseconds, MICROSECONDS_PER_MILLISECOND));
	(void)snprintf(response_interval, sizeof(response_interval), "%" PRIu64,
		       reflector_ping_interval_microseconds / target_count /
			       MICROSECONDS_PER_MILLISECOND);

	/* Up to 13 fixed arguments besides the targets. */
	if (pinger_command_init(&command, prefix, extra_arguments, 13U + target_count,
				PINGER_METHOD_FPING, error, error_size) != 0)
		return -1;
	pinger_command_add(&command, FPING_PATH);
	for (index = 0U; index < command.extra.we_wordc; index++) {
		pinger_command_add(&command, command.extra.we_wordv[index]);
		if (selects_interface(command.extra.we_wordv[index]))
			interface_selected = true;
	}
	/* Keep the SQM interface default, but honor an explicit routing override. */
	if (!interface_selected) {
		pinger_command_add(&command, FPING_INTERFACE_SHORT);
		pinger_command_add(&command, interface);
	}
	pinger_command_add(&command, FPING_TIMESTAMP);
	pinger_command_add(&command, FPING_LOOP);
	pinger_command_add(&command, FPING_PERIOD);
	pinger_command_add(&command, period);
	pinger_command_add(&command, FPING_INTERVAL);
	pinger_command_add(&command, response_interval);
	pinger_command_add(&command, FPING_TIMEOUT);
	pinger_command_add(&command, DEFAULT_FPING_TIMEOUT_MILLISECONDS);
	if (icmp_timestamps)
		pinger_command_add(&command, FPING_ICMP_TIMESTAMP);
	for (index = 0U; index < target_count; index++)
		pinger_command_add(&command, targets[index]);

	if (start_child(&latency->children[0], command.argv, PINGER_METHOD_FPING, error,
			error_size) == 0) {
		latency->ops = icmp_timestamps ? &fping_ts_ops : &fping_ops;
		latency->active = true;
		latency->child_count = 1U;
		ret = 0;
	}
	pinger_command_free(&command);
	return ret;
}
