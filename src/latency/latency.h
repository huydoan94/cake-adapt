#ifndef LATENCY_H_INCLUDED
#define LATENCY_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "common/constants.h"
#include "latency/parser.h"

/* Longest pinger output line, including its newline. */
#define LATENCY_OUTPUT_SIZE 512U

bool target_is_valid(const char *target);

/* The executable a pinger_method runs, or NULL for an unknown method. */
const char *latency_backend_executable(const char *pinger_method);

/* Like cake-autorate, refuse to start when the selected pinger cannot run. */
int latency_check_backend(const char *pinger_method, char *error, size_t error_size);

struct latency_child {
	int output_descriptor;
	/* Unreaped child; cleared once its exit is reported, so it is never re-signalled. */
	pid_t process_identifier;
	/* SIGTERM was sent by latency_close() and the exit has not been reported yet. */
	bool stopping;
	uint64_t started_us;
	uint64_t next_start_us;
	/* Borrowed from the validated configuration for the session lifetime. */
	const char *target;
};

/* What every session runs; the strings are borrowed from the validated configuration. */
struct latency_settings {
	const char *pinger_method;
	/* fping's default -I; ping_extra_args may override it. */
	const char *interface;
	const char *extra_arguments;
	const char *prefix;
	uint64_t reflector_ping_interval_us;
	uint64_t irtt_session_duration_us;
	/* IRTT sessions start aligned to ping slots counted from here. */
	uint64_t slot_origin_us;
};

struct pinger_ops;

struct latency {
	/* The backend of settings.pinger_method: fping_ops, fping_ts_ops or irtt_ops. */
	const struct pinger_ops *ops;
	struct latency_settings settings;
	bool active;
	struct latency_child children[CONFIG_MAX_REFLECTORS];
	size_t child_count;
};

enum latency_probe_result {
	LATENCY_PROBE_SUCCESS,
	LATENCY_PROBE_TIMEOUT,
	/* A line that carries no sample, such as IRTT's banner. */
	LATENCY_PROBE_PENDING,
	LATENCY_PROBE_RESTART,
	/* A child stopped by latency_close() has been reaped. */
	LATENCY_PROBE_STOPPED,
	LATENCY_PROBE_ERROR
};

enum latency_line_result {
	LATENCY_LINE_READY,
	LATENCY_LINE_INCOMPLETE,
	LATENCY_LINE_TOO_LONG
};

/* settings.pinger_method must have passed latency_check_backend(). */
void latency_init(struct latency *latency, const struct latency_settings *settings);

bool latency_is_open(const struct latency *latency);

size_t latency_child_count(const struct latency *latency);

int latency_child_descriptor(const struct latency *latency, size_t child_index);

/* Any slot up to CONFIG_MAX_REFLECTORS; -1 when it owns no unreaped child. */
pid_t latency_child_process(const struct latency *latency, size_t child_index);

/* True until every child stopped by latency_close() has been reaped. */
bool latency_stopping(const struct latency *latency);

/*
 * The validated configuration bounds targets and their spacing. fping and
 * fping-ts start one process for all targets now; IRTT schedules a session per
 * target from the next ping slot after timestamp_us, started by
 * latency_start_irtt_children().
 */
int latency_open(
	struct latency *latency,
	const char *const *targets,
	size_t target_count,
	uint64_t timestamp_us,
	char *error,
	size_t error_size
);

/*
 * Closes every output and sends SIGTERM to each live child's process group
 * without waiting. The caller reaps the children, reports each exit through
 * latency_child_exited(), and opens no session while latency_stopping().
 */
void latency_close(struct latency *latency);

/* SIGKILL for stopping children that ignored SIGTERM. */
void latency_kill_stopping(struct latency *latency);

/* Close, then stop and reap every child synchronously; for shutdown only. */
void latency_stop_now(struct latency *latency);

/* Starts the IRTT sessions that are due; only while latency_irtt_start_pending(). */
int latency_start_irtt_children(
	struct latency *latency,
	uint64_t timestamp_us,
	char *error,
	size_t error_size
);

bool latency_irtt_start_pending(const struct latency *latency);

uint64_t latency_irtt_next_start_us(const struct latency *latency);

/*
 * Copies the first complete line from data, without its CR/LF, and reports
 * how many bytes it occupied. A line longer than LATENCY_OUTPUT_SIZE fails.
 */
enum latency_line_result
latency_next_line(const char *data, size_t length, char line[LATENCY_OUTPUT_SIZE], size_t *consumed);

enum latency_probe_result latency_handle_line(
	const struct latency *latency,
	size_t child_index,
	const char *line,
	struct latency_sample *sample,
	char *error,
	size_t error_size
);

/* status is the waitpid() status of the reaped child in child_index. */
enum latency_probe_result latency_child_exited(
	struct latency *latency,
	size_t child_index,
	int status,
	char *error,
	size_t error_size
);

#endif
