#ifndef LATENCY_PINGER_H_INCLUDED
#define LATENCY_PINGER_H_INCLUDED

/* Private interface between latency session ownership and pinger backends. */

#include <stddef.h>
#include <wordexp.h>

#include "latency/latency.h"

/* Room for one formatted number argument, such as an interval. */
#define PINGER_ARGUMENT_SIZE 32U

struct pinger_ops {
	/* Names the pinger in exit diagnostics. */
	const char *name;
	/* Implements latency_open() for this backend. */
	int (*open)(
		struct latency *latency,
		const char *const *targets,
		size_t target_count,
		uint64_t timestamp_microseconds,
		char *error,
		size_t error_size
	);
	enum latency_probe_result (*parse)(
		const struct latency_child *child,
		const char *line,
		struct latency_sample *sample,
		char *error,
		size_t error_size
	);
	/* After an unexpected exit, which error already describes. */
	enum latency_probe_result (*exited)(
		struct latency_child *child,
		char *error,
		size_t error_size
	);
};

extern const struct pinger_ops fping_ops;
extern const struct pinger_ops fping_ts_ops;
extern const struct pinger_ops irtt_ops;

/* A pinger's argument vector: the prefix words, then what the backend adds. */
struct pinger_command {
	wordexp_t prefix;
	wordexp_t extra;
	char **argv;
	size_t count;
};

/*
 * Expands the session's prefix and extra-argument options and allocates room
 * for their words plus fixed more arguments; argv already starts with the prefix.
 */
int pinger_command_init(
	struct pinger_command *command,
	const struct latency *latency,
	size_t fixed,
	char *error,
	size_t error_size
);

static inline void pinger_command_add(struct pinger_command *command, const char *argument)
{
	command->argv[command->count++] = (char *)argument;
}

void pinger_command_free(struct pinger_command *command);

/*
 * Starts arguments[0] in its own process group with stdout on a nonblocking
 * pipe owned by the session's child_index and stderr on /dev/null.
 */
int start_child(
	struct latency *latency,
	size_t child_index,
	char *const arguments[],
	char *error,
	size_t error_size
);

#endif
