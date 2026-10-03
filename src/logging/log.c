#define _POSIX_C_SOURCE 200809L

#include "logging/log.h"
#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
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
#define LOG_FILE_BUFFER_SIZE (16U * KIBIBYTE)

static bool log_to_stdout;
static bool log_to_syslog;
static bool debug_to_syslog;
static FILE *log_file;
static char log_path[LOG_PATH_SIZE];
static char previous_log_path[LOG_PATH_SIZE + sizeof(LOG_PREVIOUS_SUFFIX)];
static uint64_t log_opened_microseconds;
static uint64_t log_last_flush_microseconds;
static uint64_t log_maximum_age_microseconds;
static uint64_t log_maximum_size_bytes;
static uint64_t log_size_bytes;
static uint64_t log_buffer_timeout_microseconds;
static bool log_compress_exports;
static bool header_data;
static bool header_load;
static bool header_reflector;
static bool header_summary;
static bool header_tcp_queue;
static char *cpu_header;
static bool header_cpu_raw;
static bool log_maintenance_active;
static enum log_level minimum_log_level = LOG_LEVEL_INFO;
/* Like cake-autorate, hold records until the buffer timer rather than per 1 KiB. */
static char log_file_buffer[LOG_FILE_BUFFER_SIZE];
static char datetime[LOG_DATETIME_SIZE];
static time_t datetime_seconds;
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

static const char cpu_raw_header[] =
	"CPU_RAW_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME; CPU_ID;"
	" USER; NICE; SYSTEM; IDLE; IOWAIT; IRQ; SIRQ; STEAL; GUEST;"
	" GUEST_NICE";

static const struct {
	const char *record;
	int priority;
} levels[] = { [LOG_LEVEL_ERROR] = { RECORD_ERROR, LOG_ERR },
	       [LOG_LEVEL_WARNING] = { RECORD_WARNING, LOG_WARNING },
	       [LOG_LEVEL_NOTICE] = { RECORD_INFO, LOG_NOTICE },
	       [LOG_LEVEL_INFO] = { RECORD_INFO, LOG_INFO },
	       [LOG_LEVEL_DEBUG] = { RECORD_DEBUG, LOG_DEBUG } };

static uint64_t clock_microseconds(clockid_t clock_identifier)
{
	uint64_t timestamp = 0U;

	(void)read_clock_microseconds(clock_identifier, &timestamp);
	return timestamp;
}

static void write_file_line(const char *line)
{
	int written = fprintf(log_file, "%s\n", line);

	if (written < 0)
		return;
	log_size_bytes = saturating_add(log_size_bytes, (uint64_t)written);
}

static uint64_t log_realtime_microseconds(void)
{
	return clock_microseconds(CLOCK_REALTIME);
}

static void write_headers_to_file(void)
{
	if (header_data)
		write_file_line(data_header);
	if (header_load)
		write_file_line(load_header);
	if (header_reflector)
		write_file_line(reflector_header);
	if (header_summary)
		write_file_line(summary_header);
	if (header_tcp_queue)
		write_file_line(tcp_queue_header);
	if (cpu_header != NULL)
		write_file_line(cpu_header);
	if (header_cpu_raw)
		write_file_line(cpu_raw_header);
}

static bool export_log(const char *export_path, bool compress, bool include_previous)
{
	char buffer[LOG_COPY_BUFFER_SIZE];
	const char *source_paths[] = { previous_log_path, log_path };
	/* zlib's transparent mode writes plain bytes, without a gzip wrapper. */
	gzFile destination =
		gzopen(export_path, compress ? GZIP_MODE_COMPRESSED : GZIP_MODE_TRANSPARENT);
	size_t length;
	size_t index;
	bool success = true;

	if (destination == NULL)
		return false;
	for (index = include_previous ? 0U : 1U; index < 2U; index++) {
		FILE *source = fopen(source_paths[index], FILE_MODE_READ);

		if (source == NULL) {
			if (index == 0U && errno == ENOENT)
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
		success = !ferror(source) && success;
		success = fclose(source) == 0 && success;
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
	log_opened_microseconds = clock_microseconds(CLOCK_MONOTONIC);
	log_last_flush_microseconds = log_opened_microseconds;
	return 0;
}

int log_export_file(char *export_path, size_t export_path_size)
{
	struct tm local_time;
	time_t seconds = time(NULL);
	char stamp[LOG_DATETIME_SIZE];
	size_t path_length = strlen(log_path);
	int written;

	if (log_file == NULL) {
		errno = EBADF;
		return -1;
	}
	if (localtime_r(&seconds, &local_time) == NULL ||
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
	return export_log(export_path, log_compress_exports, true) ? 0 : -1;
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

static void rotate_log_file(bool maximum_age_reached)
{
	if (log_maintenance_active)
		return;
	log_maintenance_active = true;
	if (maximum_age_reached) {
		log_message(
			LOG_LEVEL_DEBUG,
			"log file maximum time: %" PRIu64
			" minutes has elapsed so flushing and rotating log file.",
			log_maximum_age_microseconds / MICROSECONDS_PER_MINUTE
		);
	} else {
		log_message(
			LOG_LEVEL_DEBUG,
			"log file size: %" PRIu64 " KB has exceeded configured maximum: %" PRIu64
			" KB so flushing and rotating log file.",
			log_size_bytes / KIBIBYTE,
			log_maximum_size_bytes / KIBIBYTE
		);
	}
	if (fflush(log_file) == 0 && export_log(previous_log_path, false, false))
		(void)truncate_log_file();
	log_maintenance_active = false;
}

void log_tick(void)
{
	uint64_t timestamp_microseconds;

	if (log_file == NULL || log_maintenance_active)
		return;
	timestamp_microseconds = clock_microseconds(CLOCK_MONOTONIC);
	if (log_buffer_timeout_microseconds > 0U &&
	    timestamp_microseconds - log_last_flush_microseconds >=
		    log_buffer_timeout_microseconds) {
		(void)fflush(log_file);
		log_last_flush_microseconds = timestamp_microseconds;
	}
	if (log_maximum_age_microseconds > 0U &&
	    timestamp_microseconds - log_opened_microseconds > log_maximum_age_microseconds) {
		rotate_log_file(true);
	}
}

static void write_line(const char *line)
{
	if (log_to_stdout) {
		(void)fprintf(stdout, "%s\n", line);
		(void)fflush(stdout);
	}
	if (log_file != NULL) {
		write_file_line(line);
		if (log_buffer_timeout_microseconds == 0U)
			(void)fflush(log_file);
		if (log_maximum_size_bytes > 0U && log_size_bytes > log_maximum_size_bytes)
			rotate_log_file(false);
	}
}

/*
 * OpenWrt's libc re-reads /etc/TZ on every local-time conversion, and records
 * arrive many times per second. Convert once per second instead; a time zone
 * change therefore applies from the next second.
 */
static const char *local_datetime(time_t seconds)
{
	struct tm local_time;

	if (datetime_valid && seconds == datetime_seconds)
		return datetime;
	datetime_valid =
		localtime_r(&seconds, &local_time) != NULL &&
		strftime(datetime, sizeof(datetime), LOG_DATETIME_FORMAT, &local_time) != 0U;
	if (!datetime_valid)
		(void)snprintf(datetime, sizeof(datetime), LOG_DATETIME_FALLBACK);
	datetime_seconds = seconds;
	return datetime;
}

static void write_record_at(const char *type, const char *message, uint64_t timestamp_microseconds)
{
	char line[LOG_MESSAGE_SIZE];

	(void)snprintf(
		line,
		sizeof(line),
		"%s; %s; %" PRIu64 ".%06" PRIu64 "; %s",
		type,
		local_datetime((time_t)(timestamp_microseconds / MICROSECONDS_PER_SECOND)),
		timestamp_microseconds / MICROSECONDS_PER_SECOND,
		timestamp_microseconds % MICROSECONDS_PER_SECOND,
		message
	);
	write_line(line);
}

static void write_record(const char *type, const char *message)
{
	write_record_at(type, message, log_realtime_microseconds());
}

void log_init(const char *identifier, bool foreground)
{
	log_to_stdout = foreground;
	log_to_syslog = !foreground;
	debug_to_syslog = false;

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
	header_tcp_queue = false;

	if (log_to_syslog) {
		closelog();
		log_to_syslog = false;
	}
}

int log_set_file(
	const char *path,
	uint64_t maximum_time_minutes,
	uint64_t maximum_size_kilobytes,
	uint64_t buffer_timeout_microseconds,
	bool compress_exports
)
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
	log_opened_microseconds = clock_microseconds(CLOCK_MONOTONIC);
	log_last_flush_microseconds = log_opened_microseconds;
	log_maximum_age_microseconds = maximum_time_minutes * MICROSECONDS_PER_MINUTE;
	log_maximum_size_bytes = maximum_size_kilobytes * KIBIBYTE;
	log_size_bytes = (uint64_t)file_status.st_size;
	log_buffer_timeout_microseconds = buffer_timeout_microseconds;
	log_compress_exports = compress_exports;

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

void log_print_headers(
	bool output_processing_stats,
	bool output_load_stats,
	bool output_reflector_stats,
	bool output_summary_stats
)
{
	header_data = output_processing_stats;
	header_load = output_load_stats;
	header_reflector = output_reflector_stats;
	header_summary = output_summary_stats;
	if (output_processing_stats)
		write_line(data_header);
	if (output_load_stats)
		write_line(load_header);
	if (output_reflector_stats)
		write_line(reflector_header);
	if (output_summary_stats)
		write_line(summary_header);
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
	bool failed;

	free(cpu_header);
	cpu_header = NULL;
	header_cpu_raw = output_cpu_raw_stats;
	if (output_cpu_stats) {
		stream = open_memstream(&cpu_header, &size);
		if (stream == NULL) {
			log_message(LOG_LEVEL_WARNING, "could not allocate CPU log header");
		} else {
			(
				void
			)fputs("CPU_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME", stream);
			for (index = 0U; index < sample->count; index++) {
				const char *identifier = sample->counters[index].identifier;

				(void)fputs("; ", stream);
				while (*identifier != '\0')
					(void)fputc(toupper((unsigned char)*identifier++), stream);
				(void)fputs("_USAGE", stream);
			}
			failed = ferror(stream) != 0;
			if (fclose(stream) != 0)
				failed = true;
			if (failed) {
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

static void write_formatted_record(const char *type, const char *format, ...)
{
	char message[LOG_MESSAGE_SIZE];
	va_list arguments;

	if (!log_to_stdout && log_file == NULL)
		return;
	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	write_record(type, message);
}

static void write_timed_record(const char *type, const char *format, ...)
{
	char message[LOG_MESSAGE_SIZE];
	uint64_t processing_time_microseconds;
	int prefix_length;
	va_list arguments;

	if (!log_to_stdout && log_file == NULL)
		return;
	processing_time_microseconds = log_realtime_microseconds();
	prefix_length = snprintf(
		message,
		sizeof(message),
		"%" PRIu64 ".%06" PRIu64 "; ",
		processing_time_microseconds / MICROSECONDS_PER_SECOND,
		processing_time_microseconds % MICROSECONDS_PER_SECOND
	);
	if (prefix_length < 0 || (size_t)prefix_length >= sizeof(message))
		return;
	va_start(arguments, format);
	(void)vsnprintf(
		message + prefix_length,
		sizeof(message) - (size_t)prefix_length,
		format,
		arguments
	);
	va_end(arguments);
	write_record(type, message);
}

void log_print_tcp_queue_header(void)
{
	header_tcp_queue = true;
	write_line(tcp_queue_header);
}

void log_tcp_queue(const struct log_tcp_queue_record *record)
{
	write_timed_record(
		RECORD_TCP_QUEUE,
		"%d; %" PRId64 "; %d; %" PRId64,
		record->download_valid ? 1 : 0,
		record->download_queue_microseconds,
		record->upload_valid ? 1 : 0,
		record->upload_queue_microseconds
	);
}

void log_load(const struct log_load_record *record)
{
	write_timed_record(
		RECORD_LOAD,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
		record->download_achieved_rate_kbps,
		record->upload_achieved_rate_kbps,
		record->cake_download_rate_kbps,
		record->cake_upload_rate_kbps
	);
}

void log_data(const struct log_data_record *record)
{
	write_timed_record(
		RECORD_DATA,
		"%" PRIu64 "; %" PRIu64 "; %u; %u;"
		" %s; %s; %" PRIu64 ";"
		" %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRIu64 "; %" PRId64
		"; %" PRId64 "; %" PRId64 ";"
		" %" PRId64 "; %" PRIu64 "; %u; %" PRId64 "; %" PRIu64 "; %" PRIu64 "; %u; %" PRId64
		"; %" PRIu64 "; %" PRIu64 "; %s; %s; %" PRIu64 "; %" PRIu64,
		record->download_achieved_rate_kbps,
		record->upload_achieved_rate_kbps,
		record->download_load_percent,
		record->upload_load_percent,
		record->icmp_timestamp,
		record->reflector,
		record->sequence,
		record->download_owd_baseline_microseconds,
		record->download_owd_microseconds,
		record->download_owd_delta_ewma_microseconds,
		record->download_owd_delta_microseconds,
		record->download_adjust_delay_threshold_microseconds,
		record->upload_owd_baseline_microseconds,
		record->upload_owd_microseconds,
		record->upload_owd_delta_ewma_microseconds,
		record->upload_owd_delta_microseconds,
		record->upload_adjust_delay_threshold_microseconds,
		record->download_sum_delays,
		record->download_average_owd_delta_microseconds,
		record->download_maximum_adjust_up_threshold_microseconds,
		record->download_maximum_adjust_down_threshold_microseconds,
		record->upload_sum_delays,
		record->upload_average_owd_delta_microseconds,
		record->upload_maximum_adjust_up_threshold_microseconds,
		record->upload_maximum_adjust_down_threshold_microseconds,
		record->download_load_condition,
		record->upload_load_condition,
		record->cake_download_rate_kbps,
		record->cake_upload_rate_kbps
	);
}

void log_summary(const struct log_summary_record *record)
{
	write_formatted_record(
		RECORD_SUMMARY,
		"%" PRIu64 "; %" PRIu64 "; %u; %u; %" PRId64 ";"
		" %" PRId64 "; %s; %s; %" PRIu64 "; %" PRIu64,
		record->download_achieved_rate_kbps,
		record->upload_achieved_rate_kbps,
		record->download_sum_delays,
		record->upload_sum_delays,
		record->download_average_owd_delta_microseconds,
		record->upload_average_owd_delta_microseconds,
		record->download_load_condition,
		record->upload_load_condition,
		record->cake_download_rate_kbps,
		record->cake_upload_rate_kbps
	);
}

void log_reflector(const struct log_reflector_record *record)
{
	write_timed_record(
		RECORD_REFLECTOR,
		"%s; %" PRId64 "; %" PRId64 "; %" PRIu64 "; %" PRIu64 "; %" PRId64 "; %" PRId64
		"; %" PRId64 "; %" PRIu64 "; %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRIu64,
		record->reflector,
		record->minimum_sum_owd_baselines_microseconds,
		record->sum_owd_baselines_microseconds,
		record->sum_owd_baselines_delta_microseconds,
		record->sum_owd_baselines_delta_threshold_microseconds,
		record->minimum_download_delta_ewma_microseconds,
		record->download_delta_ewma_microseconds,
		record->download_delta_ewma_delta_microseconds,
		record->delta_ewma_delta_threshold_microseconds,
		record->minimum_upload_delta_ewma_microseconds,
		record->upload_delta_ewma_microseconds,
		record->upload_delta_ewma_delta_microseconds,
		record->delta_ewma_delta_threshold_microseconds
	);
}

void log_cpu(const struct cpu_sample *sample, const unsigned int *usage)
{
	char *message = NULL;
	size_t size;
	size_t index;
	FILE *stream = open_memstream(&message, &size);
	bool failed;

	if (stream == NULL) {
		log_message(LOG_LEVEL_WARNING, "could not allocate CPU log record");
		return;
	}
	(void)fprintf(
		stream,
		"%" PRIu64 ".%06" PRIu64,
		sample->timestamp_microseconds / MICROSECONDS_PER_SECOND,
		sample->timestamp_microseconds % MICROSECONDS_PER_SECOND
	);
	for (index = 0U; index < sample->count; index++)
		(void)fprintf(stream, "; %u", usage[index]);
	failed = ferror(stream) != 0;
	if (fclose(stream) != 0)
		failed = true;
	if (failed)
		log_message(LOG_LEVEL_WARNING, "could not finish CPU log record");
	else
		write_record(RECORD_CPU, message);
	free(message);
}

void log_cpu_raw(const struct cpu_sample *sample)
{
	size_t index;

	for (index = 0U; index < sample->count; index++) {
		const struct cpu_counter *counter = &sample->counters[index];

		write_formatted_record(
			RECORD_CPU_RAW,
			"%" PRIu64 ".%06" PRIu64 "; %s; %" PRIu64 "; %" PRIu64 "; %" PRIu64
			"; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64
			"; %" PRIu64 "; %" PRIu64,
			sample->timestamp_microseconds / MICROSECONDS_PER_SECOND,
			sample->timestamp_microseconds % MICROSECONDS_PER_SECOND,
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

void log_shaper(const char *interface, uint64_t rate_kbps)
{
	write_formatted_record(
		RECORD_SHAPER,
		"tc qdisc change root dev %s cake bandwidth %" PRIu64 "Kbit",
		interface,
		rate_kbps
	);
}

void log_system_message(const char *format, ...)
{
	char message[LOG_MESSAGE_SIZE];
	va_list arguments;
	uint64_t timestamp_microseconds = log_realtime_microseconds();

	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	if (log_to_syslog) {
		syslog(LOG_INFO,
		       "INFO: %" PRIu64 ".%06" PRIu64 " %s",
		       timestamp_microseconds / MICROSECONDS_PER_SECOND,
		       timestamp_microseconds % MICROSECONDS_PER_SECOND,
		       message);
	}
	write_record_at(RECORD_SYSLOG, message, timestamp_microseconds);
}

void log_message(enum log_level level, const char *format, ...)
{
	char message[LOG_MESSAGE_SIZE];
	va_list arguments;
	uint64_t timestamp_microseconds;
	bool send_syslog;

	if (level > minimum_log_level)
		return;
	if ((unsigned int)level >= ARRAY_SIZE(levels))
		level = LOG_LEVEL_ERROR;
	send_syslog = log_to_syslog &&
		      (level <= LOG_LEVEL_WARNING || (level == LOG_LEVEL_DEBUG && debug_to_syslog));
	if (!send_syslog && !log_to_stdout && log_file == NULL)
		return;

	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	timestamp_microseconds = log_realtime_microseconds();
	if (send_syslog) {
		syslog(levels[level].priority,
		       "%s: %" PRIu64 ".%06" PRIu64 " %s",
		       levels[level].record,
		       timestamp_microseconds / MICROSECONDS_PER_SECOND,
		       timestamp_microseconds % MICROSECONDS_PER_SECOND,
		       message);
	}
	if (log_to_stdout || log_file != NULL)
		write_record_at(levels[level].record, message, timestamp_microseconds);
}
