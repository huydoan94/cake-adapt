#define _POSIX_C_SOURCE 200809L

#include "logging/log.h"
#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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
static char datetime[LOG_DATETIME_SIZE];
static time_t datetime_sec;
static bool datetime_valid;

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

static uint64_t clock_us(clockid_t clock_identifier)
{
	uint64_t timestamp = 0U;

	(void)read_clock_us(clock_identifier, &timestamp);
	return timestamp;
}

/* "<seconds>.<microseconds>", the timestamp format of every record. */
static const char *timestamp_text(char text[TIMESTAMP_SIZE], uint64_t us)
{
	(void)snprintf(
		text,
		TIMESTAMP_SIZE,
		"%" PRIu64 ".%06" PRIu64,
		us / US_PER_SECOND,
		us % US_PER_SECOND
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
	if (headers.memory)
		write_file_line(memory_header);
	if (cpu_header != NULL)
		write_file_line(cpu_header);
	if (header_cpu_raw)
		write_file_line(cpu_raw_header);
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
	log_message(LOG_LEVEL_DEBUG, "Exporting log file with path: %s", export_path);
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
		log_message(
			LOG_LEVEL_DEBUG,
			"log file maximum time: %" PRIu64
			" minutes has elapsed so flushing and rotating log file.",
			log_maximum_age_us / US_PER_MINUTE
		);
	} else {
		log_message(
			LOG_LEVEL_DEBUG,
			"log file size: %" PRIu64 " KB has exceeded configured maximum: %" PRIu64
			" KB so flushing and rotating log file.",
			byte_to_kbyte(log_size_bytes),
			byte_to_kbyte(log_maximum_size_bytes)
		);
	}
	/* The active log becomes the previous one, replacing it. */
	if (fflush(log_file) == 0 && export_log(previous_log_path, GZIP_MODE_TRANSPARENT, NULL))
		(void)truncate_log_file();
	log_maintenance_active = false;
}

void log_tick(void)
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

static void write_line(const char *line)
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

/*
 * OpenWrt's libc re-reads /etc/TZ on every local-time conversion, and records
 * arrive many times per second. Convert once per second instead; a time zone
 * change therefore applies from the next second.
 */
static const char *local_datetime(time_t sec)
{
	struct tm local_time;

	if (datetime_valid && sec == datetime_sec)
		return datetime;
	datetime_valid = localtime_r(&sec, &local_time) != NULL &&
			 strftime(datetime, sizeof(datetime), LOG_DATETIME_FORMAT, &local_time) !=
				 0U;
	if (!datetime_valid)
		(void)snprintf(datetime, sizeof(datetime), LOG_DATETIME_FALLBACK);
	datetime_sec = sec;
	return datetime;
}

static void write_record_at(const char *type, const char *message, uint64_t timestamp_us)
{
	char line[LOG_MESSAGE_SIZE];
	char stamp[TIMESTAMP_SIZE];

	(void)snprintf(
		line,
		sizeof(line),
		"%s; %s; %s; %s",
		type,
		local_datetime((time_t)(timestamp_us / US_PER_SECOND)),
		timestamp_text(stamp, timestamp_us),
		message
	);
	write_line(line);
}

static void write_record(const char *type, const char *message)
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

void log_close(void)
{
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
	headers = *records;
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

	free(cpu_header);
	cpu_header = NULL;
	header_cpu_raw = output_cpu_raw_stats;
	if (output_cpu_stats) {
		stream = open_memstream(&cpu_header, &size);
		if (stream == NULL) {
			log_message(LOG_LEVEL_WARNING, "could not allocate CPU log header");
		} else {
			(void)fputs(cpu_header_prefix, stream);
			for (index = 0U; index < sample->count; index++) {
				const char *identifier = sample->counters[index].identifier;

				(void)fputs("; ", stream);
				while (*identifier != '\0')
					(void)fputc(toupper((unsigned char)*identifier++), stream);
				(void)fputs("_USAGE", stream);
			}
			if (!close_stream(stream)) {
				free(cpu_header);
				cpu_header = NULL;
				log_message(LOG_LEVEL_WARNING, "could not finish CPU log header");
			} else {
				write_line(cpu_header);
			}
		}
	}
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

static void write_formatted_record(const char *type, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

static void write_formatted_record(const char *type, const char *format, ...)
{
	va_list arguments;

	if (!log_to_stdout && log_file == NULL)
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

	if (!log_to_stdout && log_file == NULL)
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
			counter->user,
			counter->nice,
			counter->system,
			counter->idle,
			counter->iowait,
			counter->irq,
			counter->softirq,
			counter->steal,
			counter->guest,
			counter->guest_nice
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

/* "<record>: <seconds>.<microseconds> <message>", as cake-autorate sends it. */
static void
write_syslog(int priority, const char *record, uint64_t timestamp_us, const char *message)
{
	char stamp[TIMESTAMP_SIZE];

	syslog(priority, "%s: %s %s", record, timestamp_text(stamp, timestamp_us), message);
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
	if (!send_syslog && !send_stderr && !log_to_stdout && log_file == NULL)
		return;

	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	timestamp_us = log_realtime_us();
	if (send_syslog)
		write_syslog(levels[level].priority, levels[level].record, timestamp_us, message);
	if (send_stderr)
		(void)fprintf(stderr, "%s: %s\n", levels[level].record, message);
	if (log_to_stdout || log_file != NULL)
		write_record_at(levels[level].record, message, timestamp_us);
}
