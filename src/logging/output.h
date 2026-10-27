#ifndef LOGGING_OUTPUT_H_INCLUDED
#define LOGGING_OUTPUT_H_INCLUDED

/*
 * Private to logging/: how its parts reach each other. log.c formats messages
 * and sends lines on; file.c writes them to the log file and rotates, exports
 * and resets it; writer.c moves that writing to its own thread; records.c
 * formats the structured records and keeps the headers a file starts with.
 */

#include "logging/log.h"
#include "constants/logging.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* log.c: destinations, time, and the lines every part writes. */

bool log_stdout_enabled(void);

/* No record goes anywhere: no terminal, log file or MQTT, so formatting one is wasted. */
bool log_all_disabled(void);

uint64_t log_clock_us(clockid_t clock_identifier);

uint64_t log_realtime_us(void);

/* "<seconds>.<microseconds>", the timestamp format of every record. */
const char *log_timestamp_text(char text[LOG_TIMESTAMP_SIZE], uint64_t us);

/* Closes stream; false when writing to it or closing it failed. */
bool log_close_stream(FILE *stream);

/* A line for the file and stdout: to the writer thread, or written here without one. */
void log_write_line(const char *line);

/* "<type>; <local date-time>; <seconds>.<microseconds>; <message>", now. */
void log_write_record(const char *type, const char *message);

/*
 * A message from the writing side itself (rotation, export, reset,
 * overwritten lines): written directly, never queued.
 */
void log_note(enum log_level level, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* Export or reset the log file now, on the writing side, noting a failure. */
void log_export_now(void);
void log_reset_now(void);

/* file.c: the log file. */

bool file_is_open(void);

uint64_t file_buffer_timeout_us(void);

/* Writes a line to stdout and the file, on the writing side. */
void file_sink_line(const char *line);

/* Writes out buffered lines and rotates by age, when due. */
void file_tick(void);

void file_flush(void);

void file_close(void);

/* records.c: the record headers. */

/* Emits the headers set so far, as a rotated or reset file starts with them. */
void records_write_saved_headers(void (*emit)(const char *));

void records_clear(void);

/* writer.c: the writer thread. */

/*
 * What the main thread asks the writer thread to do, in order. A line goes to
 * the file and stdout, a syslog entry to syslog; export and reset are the
 * SIGUSR1 and SIGUSR2 requests.
 */
enum writer_entry_kind {
	WRITER_ENTRY_LINE,
	WRITER_ENTRY_SYSLOG,
	WRITER_ENTRY_EXPORT,
	WRITER_ENTRY_RESET,
};

bool writer_running(void);

/*
 * Copies one entry into the writer's queue, the main thread never waiting for
 * the writer: when the queue is full, the oldest entries make room.
 */
void writer_queue(enum writer_entry_kind kind, int priority, const char *text);

/*
 * Lets the writer drain the queue, within a bound. A writer still stuck (in a
 * syslog call to a logd that stopped reading) is left behind, with its state
 * and the file, as the process is about to exit; logging goes back to direct
 * writes and leaves the file alone.
 */
bool writer_stop(void);

#endif
