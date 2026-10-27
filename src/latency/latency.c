#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/children.h"
#include "latency/pinger.h"
#include "common/error.h"
#include "constants/latency.h"
#include "constants/options.h"
#include "constants/text.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

void latency_init(struct latency *latency, const struct latency_settings *settings)
{
	size_t index;

	*latency = (struct latency){
		.ops = pinger_child_ops(settings->method),
		.settings = *settings,
	};
	native_init(&latency->native);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct latency_child *child = &latency->children[index];

		child->output_descriptor = -1;
		child->process_identifier = -1;
	}
}

bool latency_target_valid(const char *target)
{
	size_t length = strlen(target);

	/* An address or hostname; it never starts with '-', so fping cannot read it as an option. */
	return length > 0U && length < LATENCY_TARGET_SIZE &&
	       (target[0] == ':' || isalnum((unsigned char)target[0])) &&
	       strspn(target, TARGET_CHARACTERS) == length;
}

bool latency_is_open(const struct latency *latency)
{
	return latency->active;
}

void latency_close(struct latency *latency)
{
	size_t index;

	native_close(&latency->native);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct latency_child *child = &latency->children[index];

		if (child->output_descriptor >= 0)
			(void)close(child->output_descriptor);
		child->output_descriptor = -1;
		child->target = NULL;
		/* A prefix may launch the pinger as a child; terminate the owned group too. */
		if (child->process_identifier > 0 && !child->stopping) {
			(void)kill(-child->process_identifier, SIGTERM);
			child->stopping = true;
		}
	}
	latency->active = false;
	latency->child_count = 0U;
}

int latency_open(
	struct latency *latency,
	const char *const *targets,
	size_t target_count,
	uint64_t timestamp_us,
	char *error,
	size_t error_size
)
{
	const struct latency_settings *settings = &latency->settings;

	if (pinger_is_native(settings->method)) {
		if (native_open(
			    &latency->native,
			    pinger_icmp_mode(settings->method),
			    settings->interface,
			    targets,
			    target_count,
			    settings->reflector_ping_interval_us,
			    timestamp_us,
			    error,
			    error_size
		    ) != 0)
			return -1;
		latency->active = true;
		return 0;
	}
	return latency->ops->open(latency, targets, target_count, timestamp_us, error, error_size);
}

enum latency_line_result
latency_next_line(const char *data, size_t length, char line[LATENCY_OUTPUT_SIZE], size_t *consumed)
{
	const char *newline =
		memchr(data, '\n', length < LATENCY_OUTPUT_SIZE ? length : LATENCY_OUTPUT_SIZE);
	size_t line_length;

	if (newline == NULL) {
		return length < LATENCY_OUTPUT_SIZE ? LATENCY_LINE_INCOMPLETE :
						      LATENCY_LINE_TOO_LONG;
	}
	line_length = (size_t)(newline - data);
	*consumed = line_length + 1U;
	if (line_length > 0U && data[line_length - 1U] == '\r')
		line_length--;
	/* The newline occupies one of the LATENCY_OUTPUT_SIZE bytes, leaving room for NUL. */
	memcpy(line, data, line_length);
	line[line_length] = '\0';
	return LATENCY_LINE_READY;
}

enum latency_probe_result latency_handle_line(
	const struct latency *latency,
	size_t child_index,
	const char *line,
	struct latency_sample *sample,
	char *error,
	size_t error_size
)
{
	return latency->ops->parse(&latency->children[child_index], line, sample, error, error_size);
}
