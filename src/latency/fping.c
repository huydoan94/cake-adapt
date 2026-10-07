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

static enum latency_probe_result
probe_result(enum latency_fping_line_result parsed, const char *line, char *error, size_t error_size)
{
	if (parsed == LATENCY_FPING_LINE_INVALID) {
		error_set(error, error_size, "unexpected fping output: %.160s", line);
		return LATENCY_PROBE_ERROR;
	}
	return parsed == LATENCY_FPING_LINE_SAMPLE ? LATENCY_PROBE_SUCCESS : LATENCY_PROBE_TIMEOUT;
}

static enum latency_probe_result fping_parse(
	const struct latency_child *child,
	const char *line,
	struct latency_sample *sample,
	char *error,
	size_t error_size
)
{
	(void)child;
	return probe_result(parse_fping_line(line, sample), line, error, error_size);
}

static enum latency_probe_result fping_ts_parse(
	const struct latency_child *child,
	const char *line,
	struct latency_sample *sample,
	char *error,
	size_t error_size
)
{
	(void)child;
	return probe_result(parse_fping_timestamp_line(line, sample), line, error, error_size);
}

/* fping runs until stopped, so any exit is a failure. */
static enum latency_probe_result
fping_exited(struct latency_child *child, char *error, size_t error_size)
{
	(void)child;
	(void)error;
	(void)error_size;
	return LATENCY_PROBE_ERROR;
}

/* -I, --iface or --iface=NAME: the user chose the interface. */
static bool selects_interface(const char *word)
{
	return strncmp(word, FPING_INTERFACE_SHORT, strlen(FPING_INTERFACE_SHORT)) == 0 ||
	       strcmp(word, FPING_INTERFACE_LONG) == 0 ||
	       strncmp(word, FPING_INTERFACE_LONG_PREFIX, strlen(FPING_INTERFACE_LONG_PREFIX)) == 0;
}

/* One fping for all targets; fping-ts adds ICMP timestamp requests (type 13). */
static int fping_open(
	struct latency *latency,
	const char *const *targets,
	size_t target_count,
	uint64_t timestamp_us,
	char *error,
	size_t error_size
)
{
	const struct latency_settings *settings = &latency->settings;
	uint64_t interval = settings->reflector_ping_interval_us;
	struct pinger_command command;
	char period[PINGER_ARGUMENT_SIZE];
	char response_interval[PINGER_ARGUMENT_SIZE];
	bool interface_selected = false;
	size_t index;
	int ret;

	(void)timestamp_us;
	(
		void
	)snprintf(period, sizeof(period), "%" PRIu64, rounded_divide(interval, US_PER_MILLISECOND));
	(void)snprintf(
		response_interval,
		sizeof(response_interval),
		"%" PRIu64,
		rounded_divide(interval, target_count * US_PER_MILLISECOND)
	);

	/* Up to 13 fixed arguments besides the targets. */
	if (pinger_command_init(&command, latency, 13U + target_count, error, error_size) != 0)
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
		pinger_command_add(&command, settings->interface);
	}
	pinger_command_add(&command, FPING_TIMESTAMP);
	pinger_command_add(&command, FPING_LOOP);
	pinger_command_add(&command, FPING_PERIOD);
	pinger_command_add(&command, period);
	pinger_command_add(&command, FPING_INTERVAL);
	pinger_command_add(&command, response_interval);
	pinger_command_add(&command, FPING_TIMEOUT);
	pinger_command_add(&command, DEFAULT_FPING_TIMEOUT_MS);
	if (latency->ops == &fping_ts_ops)
		pinger_command_add(&command, FPING_ICMP_TIMESTAMP);
	for (index = 0U; index < target_count; index++)
		pinger_command_add(&command, targets[index]);

	ret = start_child(latency, 0U, command.argv, error, error_size);
	if (ret == 0) {
		latency->active = true;
		latency->child_count = 1U;
	}
	pinger_command_free(&command);
	return ret;
}

const struct pinger_ops fping_ops = {
	.name = PINGER_METHOD_FPING,
	.open = fping_open,
	.parse = fping_parse,
	.exited = fping_exited,
};

const struct pinger_ops fping_ts_ops = {
	.name = PINGER_METHOD_FPING,
	.open = fping_open,
	.parse = fping_ts_parse,
	.exited = fping_exited,
};
