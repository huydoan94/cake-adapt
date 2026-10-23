#ifndef LOG_H_INCLUDED
#define LOG_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum log_level {
	LOG_LEVEL_ERROR,
	LOG_LEVEL_WARNING,
	LOG_LEVEL_NOTICE,
	LOG_LEVEL_INFO,
	LOG_LEVEL_DEBUG
};

void log_init(const char *identifier, bool foreground);

/*
 * Stops logging: drains the writer within a bound and closes the file and
 * syslog. False when the writer is stuck (logd or storage not responding);
 * the caller then exits with _exit(), as a normal exit would wait for it.
 */
bool log_close(void);

/* Rotation, buffering and export of the detailed log file; zero disables a limit. */
struct log_file_settings {
	uint64_t maximum_time_us;
	uint64_t maximum_size_bytes;
	uint64_t buffer_timeout_us;
	bool compress_exports;
};

int log_set_file(const char *path, const struct log_file_settings *settings);

void log_set_level(enum log_level level);

void log_tick(void);

int log_export_file(char *export_path, size_t export_path_size);

int log_reset_file(void);

/*
 * Moves every write to the log file, stdout and syslog, with rotation,
 * export and reset, to a writer thread: the caller only queues lines and
 * never waits for storage or logd. The queue is a bounded FIFO: when it is
 * full, the oldest lines make room for the newest and are counted, and the
 * log says how many were overwritten. Without it (a configuration check, the
 * tests), logging writes directly.
 */
int log_start_writer(char *error, size_t error_size);

/* SIGUSR1 and SIGUSR2: export or reset the log file, on the writer when it runs. */
void log_request_export(void);
void log_request_reset(void);

void log_set_debug_syslog(bool enabled);

/* Also writes warnings and errors to stderr, for a configuration check run by hand. */
void log_problems_to_stderr(void);

void log_system_message(const char *format, ...) __attribute__((format(printf, 1, 2)));

void log_message(enum log_level level, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

#endif
