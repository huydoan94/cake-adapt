#define _POSIX_C_SOURCE 200809L

#include "logging/log.h"
#include "logging/mqtt.h"
#include "common/constants.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define LOG_MESSAGE_SIZE 2048U
#define LOG_COPY_BUFFER_SIZE 4096U
#define LOG_FILE_BUFFER_SIZE (16U * KILOBYTE)
/* UINT64_MAX seconds, a point, six digits and the terminating NUL. */
#define TIMESTAMP_SIZE 28U
/* Room for queued lines while the writer thread is slower than the records. */
#define LOG_QUEUE_BYTES (128U * KILOBYTE)
/* The queue fill at which a line wakes the writer instead of waiting for its timer. */
#define LOG_QUEUE_WAKE_BYTES (LOG_QUEUE_BYTES / 2U)
/* The writer's longest sleep with nothing queued, between maintenance checks. */
#define LOG_WRITER_IDLE_US SECOND
/* How long stopping waits for the writer to drain the queue. */
#define LOG_WRITER_STOP_US (2U * SECOND)

static bool log_to_stdout;
static bool log_to_syslog;
/* Warnings and errors are also written to stderr, for an interactive check. */
static bool problems_to_stderr;
static bool debug_to_syslog;
static FILE *log_file;
static char log_path[LOG_PATH_SIZE];
static char previous_log_path[LOG_PATH_SIZE + sizeof(LOG_PREVIOUS_SUFFIX)];
static uint64_t log_opened_us;
static uint64_t log_last_flush_us;
static uint64_t log_maximum_age_us;
static uint64_t log_maximum_size_bytes;
static uint64_t log_size_bytes;
static uint64_t log_buffer_timeout_us;
static bool log_compress_exports;
static struct log_records headers;
static char *cpu_header;
static bool header_cpu_raw;
static bool log_maintenance_active;
static enum log_level minimum_log_level = LOG_LEVEL_INFO;
/* Like cake-autorate, hold records until the buffer timer rather than per 1 KiB. */
static char log_file_buffer[LOG_FILE_BUFFER_SIZE];
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
/* The record headers, which the writer copies into a rotated or reset file. */
static pthread_mutex_t header_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * What the main thread asks the writer thread to do, in order. A line goes to
 * the file and stdout, a syslog entry to syslog; export and reset are the
 * SIGUSR1 and SIGUSR2 requests.
 */
enum entry_kind {
	ENTRY_LINE,
	ENTRY_SYSLOG,
	ENTRY_EXPORT,
	ENTRY_RESET,
};

struct entry_header {
	uint8_t kind;
	uint8_t priority;
	uint16_t bytes;
};

/*
 * The writer thread (log_start_writer): every write to the log file, stdout
 * and syslog, rotation, export and reset happen there, so slow storage or a
 * stuck logd never holds up the main loop. The main thread only queues, into
 * a bounded FIFO: when it is full, the oldest entries make room for the new
 * one and are counted, so the newest records are always kept. The writer
 * reports the count where they went missing, before the oldest entry left.
 * An overwritten export or reset request is still carried out.
 */
struct log_writer {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	unsigned char queue[LOG_QUEUE_BYTES];
	size_t first;
	size_t used;
	/* Overwritten since the writer last took an entry, all older than first. */
	uint64_t overwritten_lines;
	uint64_t overwritten_syslog;
	bool export_pending;
	bool reset_pending;
	bool stop;
};

static struct log_writer *writer;

/* cake-autorate 3.3.0-PRERELEASE (ac75f493) analyzer schemas. */
static const char data_header[] = "DATA_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
				  " DL_ACHIEVED_RATE_KBPS; UL_ACHIEVED_RATE_KBPS; DL_LOAD_PERCENT;"
				  " UL_LOAD_PERCENT; ICMP_TIMESTAMP; REFLECTOR; SEQUENCE;"
				  " DL_OWD_BASELINE; DL_OWD_US; DL_OWD_DELTA_EWMA_US;"
				  " DL_OWD_DELTA_US; DL_ADJ_DELAY_THR; UL_OWD_BASELINE; UL_OWD_US;"
				  " UL_OWD_DELTA_EWMA_US; UL_OWD_DELTA_US; UL_ADJ_DELAY_THR;"
				  " DL_SUM_DELAYS; DL_AVG_OWD_DELTA_US;"
				  " DL_ADJ_MAX_ADJUST_UP_THR_US; DL_ADJ_MAX_ADJUST_DOWN_THR_US;"
				  " UL_SUM_DELAYS; UL_AVG_OWD_DELTA_US;"
				  " UL_ADJ_MAX_ADJUST_UP_THR_US; UL_ADJ_MAX_ADJUST_DOWN_THR_US;"
				  " DL_LOAD_CONDITION; UL_LOAD_CONDITION; CAKE_DL_RATE_KBPS;"
				  " CAKE_UL_RATE_KBPS";

static const char load_header[] = "LOAD_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
				  " DL_ACHIEVED_RATE_KBPS; UL_ACHIEVED_RATE_KBPS;"
				  " CAKE_DL_RATE_KBPS; CAKE_UL_RATE_KBPS";

static const char summary_header[] =
	"SUMMARY_HEADER; LOG_DATETIME; LOG_TIMESTAMP; DL_ACHIEVED_RATE_KBPS;"
	" UL_ACHIEVED_RATE_KBPS; DL_SUM_DELAYS; UL_SUM_DELAYS;"
	" DL_AVG_OWD_DELTA_US; UL_AVG_OWD_DELTA_US; DL_LOAD_CONDITION;"
	" UL_LOAD_CONDITION; CAKE_DL_RATE_KBPS; CAKE_UL_RATE_KBPS";

static const char reflector_header[] =
	"REFLECTOR_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US; REFLECTOR;"
	" MIN_SUM_OWD_BASELINES_US; SUM_OWD_BASELINES_US;"
	" SUM_OWD_BASELINES_DELTA_US; SUM_OWD_BASELINES_DELTA_THR_US;"
	" MIN_DL_DELTA_EWMA_US; DL_DELTA_EWMA_US; DL_DELTA_EWMA_DELTA_US;"
	" DL_DELTA_EWMA_DELTA_THR; MIN_UL_DELTA_EWMA_US; UL_DELTA_EWMA_US;"
	" UL_DELTA_EWMA_DELTA_US; UL_DELTA_EWMA_DELTA_THR";

/* Not in cake-autorate; analyzers that do not know it skip the record. */
static const char tcp_queue_header[] =
	"TCP_QUEUE_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
	" DL_QUEUE_VALID; DL_QUEUE_US; UL_QUEUE_VALID; UL_QUEUE_US";

/* Not in cake-autorate: TCP timestamp injection counters since it was loaded. */
static const char tcp_inject_header[] =
	"TCP_INJECT_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US; INJECTED; SKIPPED;"
	" ACCEPTED; STALLED";

/* Not in cake-autorate: the daemon's own memory use, in kilobytes. */
static const char memory_header[] = "MEMORY_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
				    " RSS_KB; PEAK_RSS_KB; RSS_ANON_KB; DATA_KB";

/* Followed by one "<CPU>_USAGE" column per counter line of /proc/stat. */
static const char cpu_header_prefix[] = "CPU_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME";

static const char cpu_raw_header[] =
	"CPU_RAW_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME; CPU_ID;"
	" USER; NICE; SYSTEM; IDLE; IOWAIT; IRQ; SIRQ; STEAL; GUEST;"
	" GUEST_NICE";

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

static void note(enum log_level level, const char *format, ...)
	__attribute__((format(printf, 2, 3)));
static void export_now(void);
static void reset_now(void);

static uint64_t clock_us(clockid_t clock_identifier)
{
	uint64_t timestamp_us = 0U;

	(void)read_clock_us(clock_identifier, &timestamp_us);
	return timestamp_us;
}

/* "<seconds>.<microseconds>", the timestamp format of every record. */
static const char *timestamp_text(char text[TIMESTAMP_SIZE], uint64_t us)
{
	(void)snprintf(
		text,
		TIMESTAMP_SIZE,
		"%" PRIu64 ".%06" PRIu64,
		us / MICROSECONDS_PER_SECOND,
		us % MICROSECONDS_PER_SECOND
	);
	return text;
}

/* Closes stream; false when writing to it or closing it failed. */
static bool close_stream(FILE *stream)
{
	bool written = ferror(stream) == 0;

	return fclose(stream) == 0 && written;
}

static void write_file_line(const char *line)
{
	int written = fprintf(log_file, "%s\n", line);

	if (written < 0)
		return;
	log_size_bytes = saturating_add(log_size_bytes, (uint64_t)written);
}

static uint64_t log_realtime_us(void)
{
	return clock_us(CLOCK_REALTIME);
}

static void write_headers_to_file(void)
{
	/* Rotation and reset run in the writer while the main thread may set headers. */
	(void)pthread_mutex_lock(&header_lock);
	if (headers.data)
		write_file_line(data_header);
	if (headers.load)
		write_file_line(load_header);
	if (headers.reflector)
		write_file_line(reflector_header);
	if (headers.summary)
		write_file_line(summary_header);
	if (headers.tcp_queue)
		write_file_line(tcp_queue_header);
	if (headers.tcp_inject)
		write_file_line(tcp_inject_header);
	if (headers.memory)
		write_file_line(memory_header);
	if (cpu_header != NULL)
		write_file_line(cpu_header);
	if (header_cpu_raw)
		write_file_line(cpu_raw_header);
	(void)pthread_mutex_unlock(&header_lock);
}

/*
 * Copies previous, unless it is NULL or does not exist, then the active log
 * into export_path. mode is a gzopen() mode; zlib's transparent mode writes
 * plain bytes without a gzip wrapper.
 */
static bool export_log(const char *export_path, const char *mode, const char *previous)
{
	char buffer[LOG_COPY_BUFFER_SIZE];
	const char *source_paths[] = { previous, log_path };
	gzFile destination = gzopen(export_path, mode);
	size_t length;
	size_t index;
	bool success = true;

	if (destination == NULL)
		return false;
	for (index = 0U; index < ARRAY_SIZE(source_paths); index++) {
		FILE *source;

		if (source_paths[index] == NULL)
			continue;
		source = fopen(source_paths[index], FILE_MODE_READ);
		if (source == NULL) {
			if (source_paths[index] == previous && errno == ENOENT)
				continue;
			success = false;
			break;
		}
		while ((length = fread(buffer, 1U, sizeof(buffer), source)) > 0U) {
			if (gzwrite(destination, buffer, (unsigned int)length) != (int)length) {
				success = false;
				break;
			}
		}
		success = close_stream(source) && success;
		if (!success)
			break;
	}
	return gzclose_w(destination) == Z_OK && success;
}

static int truncate_log_file(void)
{
	if (fflush(log_file) != 0 || ftruncate(fileno(log_file), 0) != 0 ||
	    fseeko(log_file, 0, SEEK_SET) != 0) {
		return -1;
	}
	log_size_bytes = 0U;
	write_headers_to_file();
	(void)fflush(log_file);
	log_opened_us = clock_us(CLOCK_MONOTONIC);
	log_last_flush_us = log_opened_us;
	return 0;
}

int log_export_file(char *export_path, size_t export_path_size)
{
	struct tm local_time;
	time_t sec = time(NULL);
	char stamp[LOG_DATETIME_SIZE];
	size_t path_length = strlen(log_path);
	const char *mode = log_compress_exports ? GZIP_MODE_COMPRESSED : GZIP_MODE_TRANSPARENT;
	int written;

	if (log_file == NULL) {
		errno = EBADF;
		return -1;
	}
	if (localtime_r(&sec, &local_time) == NULL ||
	    strftime(stamp, sizeof(stamp), EXPORT_TIME_FORMAT, &local_time) == 0U) {
		return -1;
	}
	if (path_length >= sizeof(LOG_EXTENSION) - 1U &&
	    strcmp(log_path + path_length - (sizeof(LOG_EXTENSION) - 1U), LOG_EXTENSION) == 0) {
		path_length -= sizeof(LOG_EXTENSION) - 1U;
	}
	written = snprintf(
		export_path,
		export_path_size,
		"%.*s_%s" LOG_EXTENSION "%s",
		(int)path_length,
		log_path,
		stamp,
		log_compress_exports ? GZIP_EXTENSION : EMPTY_STRING
	);
	if (written < 0 || (size_t)written >= export_path_size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	note(LOG_LEVEL_DEBUG, "Exporting log file with path: %s", export_path);
	if (fflush(log_file) != 0)
		return -1;
	return export_log(export_path, mode, previous_log_path) ? 0 : -1;
}

int log_reset_file(void)
{
	FILE *previous;

	if (log_file == NULL) {
		errno = EBADF;
		return -1;
	}
	previous = fopen(previous_log_path, FILE_MODE_WRITE);
	if (previous == NULL)
		return -1;
	(void)fclose(previous);
	return truncate_log_file();
}

enum rotation_reason {
	ROTATE_MAXIMUM_AGE,
	ROTATE_MAXIMUM_SIZE,
};

static void rotate_log_file(enum rotation_reason reason)
{
	if (log_maintenance_active)
		return;
	/* Set first: the message below is written through the log being rotated. */
	log_maintenance_active = true;
	if (reason == ROTATE_MAXIMUM_AGE) {
		note(LOG_LEVEL_DEBUG,
		     "log file maximum time: %" PRIu64
		     " minutes has elapsed so flushing and rotating log file.",
		     log_maximum_age_us / MICROSECONDS_PER_MINUTE);
	} else {
		note(LOG_LEVEL_DEBUG,
		     "log file size: %" PRIu64 " KB has exceeded configured maximum: %" PRIu64
		     " KB so flushing and rotating log file.",
		     byte_to_kbyte(log_size_bytes),
		     byte_to_kbyte(log_maximum_size_bytes));
	}
	/* The active log becomes the previous one, replacing it. */
	if (fflush(log_file) == 0 && export_log(previous_log_path, GZIP_MODE_TRANSPARENT, NULL))
		(void)truncate_log_file();
	log_maintenance_active = false;
}

/* Writes out buffered lines and rotates by age, when due. */
static void sink_tick(void)
{
	uint64_t timestamp_us;

	if (log_file == NULL || log_maintenance_active)
		return;
	timestamp_us = clock_us(CLOCK_MONOTONIC);
	if (log_buffer_timeout_us > 0U &&
	    timestamp_us - log_last_flush_us >= log_buffer_timeout_us) {
		(void)fflush(log_file);
		log_last_flush_us = timestamp_us;
	}
	if (log_maximum_age_us > 0U && timestamp_us - log_opened_us > log_maximum_age_us)
		rotate_log_file(ROTATE_MAXIMUM_AGE);
}

void log_tick(void)
{
	/* The writer thread keeps its own time. */
	if (writer == NULL)
		sink_tick();
}

/* Writes a line to stdout and the file, on the writing side. */
static void sink_line(const char *line)
{
	if (log_to_stdout) {
		(void)fprintf(stdout, "%s\n", line);
		(void)fflush(stdout);
	}
	if (log_file != NULL) {
		write_file_line(line);
		if (log_buffer_timeout_us == 0U)
			(void)fflush(log_file);
		if (log_maximum_size_bytes > 0U && log_size_bytes > log_maximum_size_bytes)
			rotate_log_file(ROTATE_MAXIMUM_SIZE);
	}
}

/* Appends one entry to the queue, which has room for it; the lock is held. */
static void
put_entry(struct log_writer *state, enum entry_kind kind, int priority, const char *text)
{
	size_t length = strnlen(text, LOG_MESSAGE_SIZE - 1U) + 1U;
	struct entry_header header = {
		.kind = (uint8_t)kind,
		.priority = (uint8_t)priority,
		.bytes = (uint16_t)length,
	};
	const unsigned char *parts[] = { (const unsigned char *)&header,
					 (const unsigned char *)text };
	const size_t sizes[] = { sizeof(header), length };
	size_t end = (state->first + state->used) % LOG_QUEUE_BYTES;

	for (size_t part = 0U; part < ARRAY_SIZE(parts); part++) {
		size_t room = LOG_QUEUE_BYTES - end;
		size_t head = sizes[part] < room ? sizes[part] : room;

		memcpy(state->queue + end, parts[part], head);
		memcpy(state->queue, parts[part] + head, sizes[part] - head);
		end = (end + sizes[part]) % LOG_QUEUE_BYTES;
	}
	/* The text is stored NUL-terminated, even when strnlen() cut it. */
	state->queue[(end + LOG_QUEUE_BYTES - 1U) % LOG_QUEUE_BYTES] = '\0';
	state->used += sizeof(header) + length;
}

static size_t entry_bytes(const char *text)
{
	return sizeof(struct entry_header) + strnlen(text, LOG_MESSAGE_SIZE - 1U) + 1U;
}

/* Reads the oldest entry's header; the lock is held. */
static struct entry_header oldest_header(const struct log_writer *state)
{
	struct entry_header header;
	unsigned char *bytes = (unsigned char *)&header;
	size_t head = LOG_QUEUE_BYTES - state->first;

	if (head > sizeof(header))
		head = sizeof(header);
	memcpy(bytes, state->queue + state->first, head);
	memcpy(bytes + head, state->queue, sizeof(header) - head);
	return header;
}

/*
 * Removes the oldest entry to make room, the lock held: a line or syslog entry
 * is counted as overwritten, an export or reset request is kept as pending.
 */
static void overwrite_oldest(struct log_writer *state)
{
	struct entry_header header = oldest_header(state);
	size_t bytes = sizeof(header) + header.bytes;

	if (header.kind == ENTRY_EXPORT)
		state->export_pending = true;
	else if (header.kind == ENTRY_RESET)
		state->reset_pending = true;
	else if (header.kind == ENTRY_SYSLOG)
		state->overwritten_syslog++;
	else
		state->overwritten_lines++;
	state->first = (state->first + bytes) % LOG_QUEUE_BYTES;
	state->used -= bytes;
}

/*
 * Whether a new entry wakes the writer; the lock is held. Syslog entries,
 * requests, lines written out at once (no buffer timeout, or a terminal) and a
 * half-full queue do. Other lines wait for the writer's timer, at most the
 * buffer timeout, as they waited in the file buffer before: a wake per record
 * cost a thread switch per record.
 */
static bool entry_wakes_writer(const struct log_writer *state, enum entry_kind kind)
{
	return kind != ENTRY_LINE || log_buffer_timeout_us == 0U || log_to_stdout ||
	       state->used >= LOG_QUEUE_WAKE_BYTES;
}

/*
 * Copies one entry into the writer's queue, the main thread never waiting for
 * the writer: when the queue is full, the oldest entries make room.
 */
static void queue_entry(enum entry_kind kind, int priority, const char *text)
{
	struct log_writer *state = writer;
	size_t bytes = entry_bytes(text);

	(void)pthread_mutex_lock(&state->lock);
	while (LOG_QUEUE_BYTES - state->used < bytes)
		overwrite_oldest(state);
	put_entry(state, kind, priority, text);
	if (entry_wakes_writer(state, kind))
		(void)pthread_cond_signal(&state->wake);
	(void)pthread_mutex_unlock(&state->lock);
}

/* A line for the file and stdout: to the writer thread, or written here without one. */
static void write_line(const char *line)
{
	mqtt_submit(line);
	if (writer != NULL)
		queue_entry(ENTRY_LINE, 0, line);
	else
		sink_line(line);
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
	char stamp[TIMESTAMP_SIZE];

	(void)snprintf(
		line,
		LOG_MESSAGE_SIZE,
		"%s; %s; %s; %s",
		type,
		local_datetime(cache, (time_t)(timestamp_us / MICROSECONDS_PER_SECOND)),
		timestamp_text(stamp, timestamp_us),
		message
	);
}

static void write_record_at(const char *type, const char *message, uint64_t timestamp_us)
{
	char line[LOG_MESSAGE_SIZE];

	format_record(line, &record_datetime, type, message, timestamp_us);
	write_line(line);
}

static void write_record(const char *type, const char *message)
{
	write_record_at(type, message, log_realtime_us());
}

/* Copies the oldest entry out of the queue, its text into text; the lock is held. */
static struct entry_header take_entry(struct log_writer *state, char text[LOG_MESSAGE_SIZE])
{
	struct entry_header header;
	unsigned char *parts[] = { (unsigned char *)&header, (unsigned char *)text };
	size_t sizes[] = { sizeof(header), 0U };

	for (size_t part = 0U; part < ARRAY_SIZE(parts); part++) {
		size_t head;

		if (part == 1U)
			sizes[part] = header.bytes;
		head = LOG_QUEUE_BYTES - state->first;
		if (sizes[part] < head)
			head = sizes[part];
		memcpy(parts[part], state->queue + state->first, head);
		memcpy(parts[part] + head, state->queue, sizes[part] - head);
		state->first = (state->first + sizes[part]) % LOG_QUEUE_BYTES;
		state->used -= sizes[part];
	}
	return header;
}

static void write_entry(struct entry_header header, const char *text)
{
	switch ((enum entry_kind)header.kind) {
	case ENTRY_LINE:
		sink_line(text);
		break;
	case ENTRY_SYSLOG:
		syslog(header.priority, "%s", text);
		break;
	case ENTRY_EXPORT:
		export_now();
		break;
	case ENTRY_RESET:
		reset_now();
		break;
	}
}

/* Waits, the lock held, until an entry is queued, a stop is asked or the idle time ends. */
static void wait_for_entry(struct log_writer *state)
{
	uint64_t idle_us = log_buffer_timeout_us > 0U ?
				   min_u64(log_buffer_timeout_us, LOG_WRITER_IDLE_US) :
				   LOG_WRITER_IDLE_US;
	struct timespec deadline =
		us_to_timespec(saturating_add(clock_us(CLOCK_MONOTONIC), idle_us));

	(void)pthread_cond_timedwait(&state->wake, &state->lock, &deadline);
}

static void *writer_thread(void *argument)
{
	struct log_writer *state = argument;
	char text[LOG_MESSAGE_SIZE];

	(void)pthread_mutex_lock(&state->lock);
	for (;;) {
		struct entry_header header;
		uint64_t overwritten_lines;
		uint64_t overwritten_syslog;
		bool export_pending;
		bool reset_pending;
		bool taken;

		if (state->used == 0U && state->stop)
			break;
		if (state->used == 0U)
			wait_for_entry(state);
		/* What was overwritten is older than the entry taken now: report it first. */
		overwritten_lines = state->overwritten_lines;
		overwritten_syslog = state->overwritten_syslog;
		export_pending = state->export_pending;
		reset_pending = state->reset_pending;
		state->overwritten_lines = 0U;
		state->overwritten_syslog = 0U;
		state->export_pending = false;
		state->reset_pending = false;
		taken = state->used > 0U;
		if (taken)
			header = take_entry(state, text);
		(void)pthread_mutex_unlock(&state->lock);
		/*
		 * Overwritten requests run before the note, so a reset cannot erase
		 * it. Repeated or interleaved requests collapse into one export, then
		 * one reset: the export loses nothing either way.
		 */
		if (export_pending)
			export_now();
		if (reset_pending)
			reset_now();
		if (overwritten_lines > 0U || overwritten_syslog > 0U)
			note(LOG_LEVEL_WARNING,
			     "%" PRIu64 " older log lines and %" PRIu64
			     " syslog messages were overwritten: the log writer fell behind",
			     overwritten_lines,
			     overwritten_syslog);
		if (taken)
			write_entry(header, text);
		sink_tick();
		(void)pthread_mutex_lock(&state->lock);
	}
	(void)pthread_mutex_unlock(&state->lock);
	if (log_file != NULL)
		(void)fflush(log_file);
	return NULL;
}

int log_start_writer(char *error, size_t error_size)
{
	struct log_writer *state;
	pthread_condattr_t attributes;
	sigset_t all;
	sigset_t previous;
	int result;

	if (writer != NULL)
		return 0;
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return error_set(error, error_size, "could not allocate the log writer");
	/* Waits are timed on the monotonic clock, as the rest of the daemon. */
	if (pthread_mutex_init(&state->lock, NULL) != 0 ||
	    pthread_condattr_init(&attributes) != 0) {
		free(state);
		return error_set(error, error_size, "could not prepare the log writer");
	}
	(void)pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	result = pthread_cond_init(&state->wake, &attributes);
	(void)pthread_condattr_destroy(&attributes);
	if (result != 0) {
		(void)pthread_mutex_destroy(&state->lock);
		free(state);
		return error_set(error, error_size, "could not prepare the log writer");
	}
	/* Signals stay with the main thread's event loop. */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	result = pthread_create(&state->thread, NULL, writer_thread, state);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	if (result != 0) {
		(void)pthread_cond_destroy(&state->wake);
		(void)pthread_mutex_destroy(&state->lock);
		free(state);
		return error_set(
			error,
			error_size,
			"could not start the log writer: %s",
			strerror(result)
		);
	}
	writer = state;
	return 0;
}

/*
 * Lets the writer drain the queue, within a bound. A writer still stuck (in a
 * syslog call to a logd that stopped reading) is left behind, with its state
 * and the file, as the process is about to exit; logging goes back to direct
 * writes and leaves the file alone.
 */
static bool stop_writer(void)
{
	struct log_writer *state = writer;

	if (state == NULL)
		return true;
	writer = NULL;
	(void)pthread_mutex_lock(&state->lock);
	state->stop = true;
	(void)pthread_cond_signal(&state->wake);
	(void)pthread_mutex_unlock(&state->lock);
	if (!thread_join_within(state->thread, LOG_WRITER_STOP_US)) {
		(void)pthread_detach(state->thread);
		return false;
	}
	(void)pthread_cond_destroy(&state->wake);
	(void)pthread_mutex_destroy(&state->lock);
	free(state);
	return true;
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
	if (!stop_writer()) {
		/*
		 * The writer is stuck in syslog or on the file, holding libc's locks:
		 * closelog(), fclose() and the exit's stdio flush would wait for it.
		 * Leave everything it may still touch (file, headers, MQTT) to the
		 * process exit, which the caller makes without flushing.
		 */
		return false;
	}
	mqtt_stop();
	if (log_file != NULL) {
		(void)fclose(log_file);
		log_file = NULL;
	}
	free(cpu_header);
	cpu_header = NULL;
	header_cpu_raw = false;
	headers = (struct log_records){ 0 };

	if (log_to_syslog) {
		closelog();
		log_to_syslog = false;
	}
	return true;
}

int log_set_file(const char *path, const struct log_file_settings *settings)
{
	FILE *file;
	struct stat file_status;

	if (path == NULL || path[0] == '\0') {
		errno = EINVAL;
		return -1;
	}

	if (strlen(path) >= sizeof(log_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	/* Linux libc opens with O_CLOEXEC atomically for the 'e' mode. */
	file = fopen(path, FILE_MODE_APPEND_CLOEXEC);
	if (file == NULL)
		return -1;
	if (fstat(fileno(file), &file_status) != 0 || file_status.st_size < 0) {
		int saved_errno = errno;

		(void)fclose(file);
		errno = saved_errno == 0 ? EIO : saved_errno;
		return -1;
	}
	/* The previous stream must release the shared buffer before it is reused. */
	if (log_file != NULL)
		(void)fclose(log_file);
	(void)setvbuf(file, log_file_buffer, _IOFBF, sizeof(log_file_buffer));
	(void)snprintf(log_path, sizeof(log_path), "%s", path);
	(void)snprintf(
		previous_log_path,
		sizeof(previous_log_path),
		"%s%s",
		log_path,
		LOG_PREVIOUS_SUFFIX
	);
	log_file = file;
	log_opened_us = clock_us(CLOCK_MONOTONIC);
	log_last_flush_us = log_opened_us;
	log_maximum_age_us = settings->maximum_time_us;
	log_maximum_size_bytes = settings->maximum_size_bytes;
	log_size_bytes = (uint64_t)file_status.st_size;
	log_buffer_timeout_us = settings->buffer_timeout_us;
	log_compress_exports = settings->compress_exports;

	return 0;
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

void log_print_headers(const struct log_records *records)
{
	(void)pthread_mutex_lock(&header_lock);
	headers = *records;
	(void)pthread_mutex_unlock(&header_lock);
	if (records->data)
		write_line(data_header);
	if (records->load)
		write_line(load_header);
	if (records->reflector)
		write_line(reflector_header);
	if (records->summary)
		write_line(summary_header);
	if (records->tcp_queue)
		write_line(tcp_queue_header);
	if (records->tcp_inject)
		write_line(tcp_inject_header);
	if (records->memory)
		write_line(memory_header);
}

void log_print_cpu_headers(
	const struct cpu_sample *sample,
	bool output_cpu_stats,
	bool output_cpu_raw_stats
)
{
	size_t index;
	size_t size;
	FILE *stream;
	char *header = NULL;

	if (output_cpu_stats) {
		stream = open_memstream(&header, &size);
		if (stream == NULL) {
			log_message(LOG_LEVEL_WARNING, "could not allocate CPU log header");
		} else {
			(void)fputs(cpu_header_prefix, stream);
			for (index = 0U; index < sample->count; index++) {
				const struct cpu_counter *counter = &sample->counters[index];
				const char *identifier = counter->identifier;

				(void)fputs("; ", stream);
				while (*identifier != '\0')
					(void)fputc(toupper((unsigned char)*identifier++), stream);
				(void)fputs("_USAGE", stream);
			}
			if (!close_stream(stream)) {
				free(header);
				header = NULL;
				log_message(LOG_LEVEL_WARNING, "could not finish CPU log header");
			}
		}
	}
	/* Built first, then swapped in, so the writer never sees a half-made header. */
	(void)pthread_mutex_lock(&header_lock);
	free(cpu_header);
	cpu_header = header;
	header_cpu_raw = output_cpu_raw_stats;
	(void)pthread_mutex_unlock(&header_lock);
	if (header != NULL)
		write_line(header);
	if (output_cpu_raw_stats)
		write_line(cpu_raw_header);
}

/* prefix, when not NULL, starts the record, followed by the formatted fields. */
static void
write_record_fields(const char *type, const char *prefix, const char *format, va_list arguments)
{
	char message[LOG_MESSAGE_SIZE];
	int prefix_length = 0;

	if (prefix != NULL)
		prefix_length = snprintf(message, sizeof(message), "%s; ", prefix);
	(void)vsnprintf(
		message + prefix_length,
		sizeof(message) - (size_t)prefix_length,
		format,
		arguments
	);
	write_record(type, message);
}

/* No record goes anywhere: no terminal, log file or MQTT, so formatting one is wasted. */
static bool all_logs_disabled(void)
{
	return !log_to_stdout && log_file == NULL && !mqtt_enabled();
}

static void write_formatted_record(const char *type, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

static void write_formatted_record(const char *type, const char *format, ...)
{
	va_list arguments;

	if (all_logs_disabled())
		return;
	va_start(arguments, format);
	write_record_fields(type, NULL, format, arguments);
	va_end(arguments);
}

static void write_timed_record(const char *type, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/* Starts with the processing time, cake-autorate's PROC_TIME_US column. */
static void write_timed_record(const char *type, const char *format, ...)
{
	char stamp[TIMESTAMP_SIZE];
	va_list arguments;

	if (all_logs_disabled())
		return;
	va_start(arguments, format);
	write_record_fields(type, timestamp_text(stamp, log_realtime_us()), format, arguments);
	va_end(arguments);
}

void log_tcp_queue(const struct log_tcp_queue_record *record)
{
	write_timed_record(
		RECORD_TCP_QUEUE,
		"%d; %" PRId64 "; %d; %" PRId64,
		record->download_valid ? 1 : 0,
		record->download_queue_us,
		record->upload_valid ? 1 : 0,
		record->upload_queue_us
	);
}

void log_tcp_inject(const struct log_tcp_inject_record *record)
{
	write_timed_record(
		RECORD_TCP_INJECT,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
		record->injected,
		record->skipped,
		record->accepted,
		record->stalled
	);
}

void log_memory(const struct memory_sample *sample)
{
	write_timed_record(
		RECORD_MEMORY,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
		byte_to_kbyte(sample->rss_bytes),
		byte_to_kbyte(sample->peak_rss_bytes),
		byte_to_kbyte(sample->anonymous_bytes),
		byte_to_kbyte(sample->data_bytes)
	);
}

void log_load(const struct log_load_record *record)
{
	write_timed_record(
		RECORD_LOAD,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
		bit_to_kbit(record->download_achieved_rate_bps),
		bit_to_kbit(record->upload_achieved_rate_bps),
		bit_to_kbit(record->cake_download_rate_bps),
		bit_to_kbit(record->cake_upload_rate_bps)
	);
}

/* cake-autorate prints a ratio as a whole percent, truncated. */
static uint64_t whole_percent(uint64_t ratio_e6)
{
	return ratio_e6 / RATIO_PERCENT_E6;
}

void log_data(const struct log_data_record *record)
{
	write_timed_record(
		RECORD_DATA,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64 ";"
		" %s; %s; %" PRIu64 ";"
		" %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRIu64 "; %" PRId64
		"; %" PRId64 "; %" PRId64 ";"
		" %" PRId64 "; %" PRIu64 "; %u; %" PRId64 "; %" PRIu64 "; %" PRIu64 "; %u; %" PRId64
		"; %" PRIu64 "; %" PRIu64 "; %s; %s; %" PRIu64 "; %" PRIu64,
		bit_to_kbit(record->download_achieved_rate_bps),
		bit_to_kbit(record->upload_achieved_rate_bps),
		whole_percent(record->download_load_ratio_e6),
		whole_percent(record->upload_load_ratio_e6),
		record->icmp_timestamp,
		record->reflector,
		record->sequence,
		record->download_owd_baseline_us,
		record->download_owd_us,
		record->download_owd_delta_ewma_us,
		record->download_owd_delta_us,
		record->download_adjust_delay_threshold_us,
		record->upload_owd_baseline_us,
		record->upload_owd_us,
		record->upload_owd_delta_ewma_us,
		record->upload_owd_delta_us,
		record->upload_adjust_delay_threshold_us,
		record->download_sum_delays,
		record->download_average_owd_delta_us,
		record->download_maximum_adjust_up_threshold_us,
		record->download_maximum_adjust_down_threshold_us,
		record->upload_sum_delays,
		record->upload_average_owd_delta_us,
		record->upload_maximum_adjust_up_threshold_us,
		record->upload_maximum_adjust_down_threshold_us,
		record->download_load_condition,
		record->upload_load_condition,
		bit_to_kbit(record->cake_download_rate_bps),
		bit_to_kbit(record->cake_upload_rate_bps)
	);
}

void log_summary(const struct log_summary_record *record)
{
	write_formatted_record(
		RECORD_SUMMARY,
		"%" PRIu64 "; %" PRIu64 "; %u; %u; %" PRId64 ";"
		" %" PRId64 "; %s; %s; %" PRIu64 "; %" PRIu64,
		bit_to_kbit(record->download_achieved_rate_bps),
		bit_to_kbit(record->upload_achieved_rate_bps),
		record->download_sum_delays,
		record->upload_sum_delays,
		record->download_average_owd_delta_us,
		record->upload_average_owd_delta_us,
		record->download_load_condition,
		record->upload_load_condition,
		bit_to_kbit(record->cake_download_rate_bps),
		bit_to_kbit(record->cake_upload_rate_bps)
	);
}

void log_reflector(const struct log_reflector_record *record)
{
	write_timed_record(
		RECORD_REFLECTOR,
		"%s; %" PRId64 "; %" PRId64 "; %" PRIu64 "; %" PRIu64 "; %" PRId64 "; %" PRId64
		"; %" PRId64 "; %" PRIu64 "; %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRIu64,
		record->reflector,
		record->minimum_sum_owd_baselines_us,
		record->sum_owd_baselines_us,
		record->sum_owd_baselines_delta_us,
		record->sum_owd_baselines_delta_threshold_us,
		record->minimum_download_delta_ewma_us,
		record->download_delta_ewma_us,
		record->download_delta_ewma_delta_us,
		record->delta_ewma_delta_threshold_us,
		record->minimum_upload_delta_ewma_us,
		record->upload_delta_ewma_us,
		record->upload_delta_ewma_delta_us,
		record->delta_ewma_delta_threshold_us
	);
}

void log_cpu(const struct cpu_sample *sample, const struct cpu_busy *usage)
{
	char *message = NULL;
	char stamp[TIMESTAMP_SIZE];
	size_t size;
	size_t index;
	FILE *stream = open_memstream(&message, &size);

	if (stream == NULL) {
		log_message(LOG_LEVEL_WARNING, "could not allocate CPU log record");
		return;
	}
	(void)fputs(timestamp_text(stamp, sample->timestamp_us), stream);
	for (index = 0U; index < sample->count; index++)
		(void)fprintf(
			stream,
			"; %" PRIu64,
			whole_percent(fraction_to_ratio_e6(
				usage[index].busy_ticks,
				usage[index].total_ticks
			))
		);
	if (!close_stream(stream))
		log_message(LOG_LEVEL_WARNING, "could not finish CPU log record");
	else
		write_record(RECORD_CPU, message);
	free(message);
}

void log_cpu_raw(const struct cpu_sample *sample)
{
	char stamp[TIMESTAMP_SIZE];
	size_t index;

	(void)timestamp_text(stamp, sample->timestamp_us);
	for (index = 0U; index < sample->count; index++) {
		const struct cpu_counter *counter = &sample->counters[index];

		write_formatted_record(
			RECORD_CPU_RAW,
			"%s; %s; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64
			"; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
			stamp,
			counter->identifier,
			counter->user_ticks,
			counter->nice_ticks,
			counter->system_ticks,
			counter->idle_ticks,
			counter->iowait_ticks,
			counter->irq_ticks,
			counter->softirq_ticks,
			counter->steal_ticks,
			counter->guest_ticks,
			counter->guest_nice_ticks
		);
	}
}

void log_shaper(const char *interface, uint64_t rate_bps)
{
	write_formatted_record(
		RECORD_SHAPER,
		"tc qdisc change root dev %s cake bandwidth %" PRIu64 "Kbit",
		interface,
		bit_to_kbit(rate_bps)
	);
}

/*
 * "<record>: <seconds>.<microseconds> <message>", as cake-autorate sends it:
 * to the writer thread, or sent here without one.
 */
static void
write_syslog(int priority, const char *record, uint64_t timestamp_us, const char *message)
{
	char text[LOG_MESSAGE_SIZE];
	char stamp[TIMESTAMP_SIZE];

	(void)snprintf(
		text,
		sizeof(text),
		"%s: %s %s",
		record,
		timestamp_text(stamp, timestamp_us),
		message
	);
	if (writer != NULL)
		queue_entry(ENTRY_SYSLOG, priority, text);
	else
		syslog(priority, "%s", text);
}

/*
 * A message from the writing side itself (rotation, export, reset,
 * overwritten lines): written directly, never queued, with its own date-time
 * cache.
 */
static void note(enum log_level level, const char *format, ...)
{
	char message[LOG_MESSAGE_SIZE];
	char line[LOG_MESSAGE_SIZE];
	char stamp[TIMESTAMP_SIZE];
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
		       timestamp_text(stamp, timestamp_us),
		       message);
	format_record(line, &note_datetime, levels[level].record, message, timestamp_us);
	mqtt_submit(line);
	sink_line(line);
}

static void export_now(void)
{
	char export_path[LOG_EXPORT_PATH_SIZE];

	if (log_export_file(export_path, sizeof(export_path)) != 0)
		note(LOG_LEVEL_WARNING, "log file export failed: %s", strerror(errno));
}

static void reset_now(void)
{
	if (log_reset_file() != 0)
		note(LOG_LEVEL_WARNING, "log file reset failed: %s", strerror(errno));
}

void log_request_export(void)
{
	if (writer != NULL)
		queue_entry(ENTRY_EXPORT, 0, "");
	else
		export_now();
}

void log_request_reset(void)
{
	if (writer != NULL)
		queue_entry(ENTRY_RESET, 0, "");
	else
		reset_now();
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
