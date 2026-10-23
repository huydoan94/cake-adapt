#define _POSIX_C_SOURCE 200809L

#include "logging/log.h"
#include "logging/output.h"
#include "logging/mqtt.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "constants/logging.h"
#include "constants/units.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static bool log_to_stdout;
static bool log_to_syslog;
/* Warnings and errors are also written to stderr, for an interactive check. */
static bool problems_to_stderr;
static bool debug_to_syslog;
static enum log_level minimum_log_level = LOG_LEVEL_INFO;
/* One local-time conversion per second, per thread that formats records. */
struct datetime_cache {
	char text[LOG_DATETIME_SIZE];
	time_t sec;
	bool valid;
};

/* For records the main thread formats. */
static struct datetime_cache record_datetime;
/* For notes from the writing side: the writer thread, or the main thread without one. */
static struct datetime_cache note_datetime;

static const struct {
	const char *record;
	int priority;
} levels[] = {
	[LOG_LEVEL_ERROR] = { RECORD_ERROR, LOG_ERR },
	[LOG_LEVEL_WARNING] = { RECORD_WARNING, LOG_WARNING },
	[LOG_LEVEL_NOTICE] = { RECORD_INFO, LOG_NOTICE },
	[LOG_LEVEL_INFO] = { RECORD_INFO, LOG_INFO },
	[LOG_LEVEL_DEBUG] = { RECORD_DEBUG, LOG_DEBUG },
};

uint64_t log_clock_us(clockid_t clock_identifier)
{
	uint64_t timestamp_us = 0U;

	(void)read_clock_us(clock_identifier, &timestamp_us);
	return timestamp_us;
}

const char *log_timestamp_text(char text[LOG_TIMESTAMP_SIZE], uint64_t us)
{
	(void)snprintf(
		text,
		LOG_TIMESTAMP_SIZE,
		"%" PRIu64 ".%06" PRIu64,
		us / MICROSECONDS_PER_SECOND,
		us % MICROSECONDS_PER_SECOND
	);
	return text;
}

bool log_close_stream(FILE *stream)
{
	bool written = ferror(stream) == 0;

	return fclose(stream) == 0 && written;
}

uint64_t log_realtime_us(void)
{
	return log_clock_us(CLOCK_REALTIME);
}

bool log_stdout_enabled(void)
{
	return log_to_stdout;
}

bool all_logs_disabled(void)
{
	return !log_to_stdout && !file_is_open() && !mqtt_enabled();
}

void log_tick(void)
{
	/* The writer thread keeps its own time. */
	if (!writer_running())
		file_tick();
}

void log_write_line(const char *line)
{
	mqtt_submit(line);
	if (writer_running())
		writer_queue(ENTRY_LINE, 0, line);
	else
		file_sink_line(line);
}

/*
 * OpenWrt's libc re-reads /etc/TZ on every local-time conversion, and records
 * arrive many times per second. Convert once per second instead; a time zone
 * change therefore applies from the next second.
 */
static const char *local_datetime(struct datetime_cache *cache, time_t sec)
{
	struct tm local_time;

	if (cache->valid && sec == cache->sec)
		return cache->text;
	cache->valid =
		localtime_r(&sec, &local_time) != NULL &&
		strftime(cache->text, sizeof(cache->text), LOG_DATETIME_FORMAT, &local_time) != 0U;
	if (!cache->valid)
		(void)snprintf(cache->text, sizeof(cache->text), LOG_DATETIME_FALLBACK);
	cache->sec = sec;
	return cache->text;
}

/* "<type>; <local date-time>; <seconds>.<microseconds>; <message>", into line. */
static void format_record(
	char line[LOG_MESSAGE_SIZE],
	struct datetime_cache *cache,
	const char *type,
	const char *message,
	uint64_t timestamp_us
)
{
	char stamp[LOG_TIMESTAMP_SIZE];

	(void)snprintf(
		line,
		LOG_MESSAGE_SIZE,
		"%s; %s; %s; %s",
		type,
		local_datetime(cache, (time_t)(timestamp_us / MICROSECONDS_PER_SECOND)),
		log_timestamp_text(stamp, timestamp_us),
		message
	);
}

static void write_record_at(const char *type, const char *message, uint64_t timestamp_us)
{
	char line[LOG_MESSAGE_SIZE];

	format_record(line, &record_datetime, type, message, timestamp_us);
	log_write_line(line);
}

void log_write_record(const char *type, const char *message)
{
	write_record_at(type, message, log_realtime_us());
}

void log_init(const char *identifier, bool foreground)
{
	log_to_stdout = foreground;
	log_to_syslog = !foreground;
	debug_to_syslog = false;
	problems_to_stderr = false;

	if (log_to_syslog)
		openlog(identifier, LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

bool log_close(void)
{
	if (!writer_stop()) {
		/*
		 * The writer is stuck in syslog or on the file, holding libc's locks:
		 * closelog(), fclose() and the exit's stdio flush would wait for it.
		 * Leave everything it may still touch (file, headers, MQTT) to the
		 * process exit, which the caller makes without flushing.
		 */
		return false;
	}
	mqtt_stop();
	file_close();
	records_clear();

	if (log_to_syslog) {
		closelog();
		log_to_syslog = false;
	}
	return true;
}

void log_set_level(enum log_level level)
{
	minimum_log_level = level;
}

void log_set_debug_syslog(bool enabled)
{
	debug_to_syslog = enabled;
}

void log_problems_to_stderr(void)
{
	problems_to_stderr = true;
}

/*
 * "<record>: <seconds>.<microseconds> <message>", as cake-autorate sends it:
 * to the writer thread, or sent here without one.
 */
static void
write_syslog(int priority, const char *record, uint64_t timestamp_us, const char *message)
{
	char text[LOG_MESSAGE_SIZE];
	char stamp[LOG_TIMESTAMP_SIZE];

	(void)snprintf(
		text,
		sizeof(text),
		"%s: %s %s",
		record,
		log_timestamp_text(stamp, timestamp_us),
		message
	);
	if (writer_running())
		writer_queue(ENTRY_SYSLOG, priority, text);
	else
		syslog(priority, "%s", text);
}

/* Written directly, never queued, with its own date-time cache. */
void log_note(enum log_level level, const char *format, ...)
{
	char message[LOG_MESSAGE_SIZE];
	char line[LOG_MESSAGE_SIZE];
	char stamp[LOG_TIMESTAMP_SIZE];
	va_list arguments;
	uint64_t timestamp_us;

	if (level > minimum_log_level)
		return;
	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	timestamp_us = log_realtime_us();
	if (log_to_syslog &&
	    (level <= LOG_LEVEL_WARNING || (level == LOG_LEVEL_DEBUG && debug_to_syslog)))
		syslog(levels[level].priority,
		       "%s: %s %s",
		       levels[level].record,
		       log_timestamp_text(stamp, timestamp_us),
		       message);
	format_record(line, &note_datetime, levels[level].record, message, timestamp_us);
	mqtt_submit(line);
	file_sink_line(line);
}

void log_export_now(void)
{
	char export_path[LOG_EXPORT_PATH_SIZE];

	if (log_export_file(export_path, sizeof(export_path)) != 0)
		log_note(LOG_LEVEL_WARNING, "log file export failed: %s", strerror(errno));
}

void log_reset_now(void)
{
	if (log_reset_file() != 0)
		log_note(LOG_LEVEL_WARNING, "log file reset failed: %s", strerror(errno));
}

void log_request_export(void)
{
	if (writer_running())
		writer_queue(ENTRY_EXPORT, 0, "");
	else
		log_export_now();
}

void log_request_reset(void)
{
	if (writer_running())
		writer_queue(ENTRY_RESET, 0, "");
	else
		log_reset_now();
}

void log_system_message(const char *format, ...)
{
	char message[LOG_MESSAGE_SIZE];
	va_list arguments;
	uint64_t timestamp_us = log_realtime_us();

	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	if (log_to_syslog)
		write_syslog(LOG_INFO, RECORD_INFO, timestamp_us, message);
	write_record_at(RECORD_SYSLOG, message, timestamp_us);
}

void log_message(enum log_level level, const char *format, ...)
{
	char message[LOG_MESSAGE_SIZE];
	va_list arguments;
	uint64_t timestamp_us;
	bool send_syslog;
	bool send_stderr;

	if (level > minimum_log_level)
		return;
	if ((unsigned int)level >= ARRAY_SIZE(levels))
		level = LOG_LEVEL_ERROR;
	send_syslog = log_to_syslog &&
		      (level <= LOG_LEVEL_WARNING || (level == LOG_LEVEL_DEBUG && debug_to_syslog));
	/* In the foreground the message already reaches the terminal on stdout. */
	send_stderr = problems_to_stderr && !log_to_stdout && level <= LOG_LEVEL_WARNING;
	if (!send_syslog && !send_stderr && all_logs_disabled())
		return;

	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	timestamp_us = log_realtime_us();
	if (send_syslog)
		write_syslog(levels[level].priority, levels[level].record, timestamp_us, message);
	if (send_stderr)
		(void)fprintf(stderr, "%s: %s\n", levels[level].record, message);
	if (!all_logs_disabled())
		write_record_at(levels[level].record, message, timestamp_us);
}
