#define _POSIX_C_SOURCE 200809L

#include "logging/output.h"
#include "common/arith.h"
#include "common/convert.h"
#include "constants/logging.h"
#include "constants/text.h"
#include "constants/units.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

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
static bool log_maintenance_active;
/* Like cake-autorate, hold records until the buffer timer rather than per 1 KiB. */
static char log_file_buffer[LOG_FILE_BUFFER_SIZE];

bool file_is_open(void)
{
	return log_file != NULL;
}

uint64_t file_buffer_timeout_us(void)
{
	return log_buffer_timeout_us;
}

static void write_file_line(const char *line)
{
	int written = fprintf(log_file, "%s\n", line);

	if (written < 0)
		return;
	log_size_bytes = saturating_add(log_size_bytes, (uint64_t)written);
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
		success = log_close_stream(source) && success;
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
	records_write_saved_headers(write_file_line);
	(void)fflush(log_file);
	log_opened_us = log_clock_us(CLOCK_MONOTONIC);
	log_last_flush_us = log_opened_us;
	return 0;
}

int log_export_file(char *export_path, size_t export_path_size)
{
	struct tm local_time;
	time_t sec = time(NULL);
	char stamp[LOG_DATETIME_SIZE];
	size_t path_length = strlen(log_path);
	const size_t extension_length = sizeof(LOG_EXTENSION) - 1U;
	const char *mode = GZIP_MODE_TRANSPARENT;
	const char *compressed_extension = EMPTY_STRING;
	int written;

	if (log_file == NULL) {
		errno = EBADF;
		return -1;
	}
	if (localtime_r(&sec, &local_time) == NULL ||
	    strftime(stamp, sizeof(stamp), EXPORT_TIME_FORMAT, &local_time) == 0U) {
		return -1;
	}
	if (path_length >= extension_length &&
	    strcmp(log_path + path_length - extension_length, LOG_EXTENSION) == 0)
		path_length -= extension_length;
	if (log_compress_exports) {
		mode = GZIP_MODE_COMPRESSED;
		compressed_extension = GZIP_EXTENSION;
	}
	written = snprintf(
		export_path,
		export_path_size,
		"%.*s_%s" LOG_EXTENSION "%s",
		(int)path_length,
		log_path,
		stamp,
		compressed_extension
	);
	if (written < 0 || (size_t)written >= export_path_size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	log_note(LOG_LEVEL_DEBUG, "Exporting log file with path: %s", export_path);
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
	ROTATION_MAXIMUM_AGE,
	ROTATION_MAXIMUM_SIZE,
};

static void rotate_log_file(enum rotation_reason reason)
{
	if (log_maintenance_active)
		return;
	/* Set first: the message below is written through the log being rotated. */
	log_maintenance_active = true;
	if (reason == ROTATION_MAXIMUM_AGE) {
		log_note(
			LOG_LEVEL_DEBUG,
			"log file maximum time: %" PRIu64
			" minutes has elapsed so flushing and rotating log file.",
			log_maximum_age_us / MICROSECONDS_PER_MINUTE
		);
	} else {
		log_note(
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

void file_tick(void)
{
	uint64_t timestamp_us;

	if (log_file == NULL || log_maintenance_active)
		return;
	timestamp_us = log_clock_us(CLOCK_MONOTONIC);
	if (log_buffer_timeout_us > 0U &&
	    timestamp_us - log_last_flush_us >= log_buffer_timeout_us) {
		(void)fflush(log_file);
		log_last_flush_us = timestamp_us;
	}
	if (log_maximum_age_us > 0U && timestamp_us - log_opened_us > log_maximum_age_us)
		rotate_log_file(ROTATION_MAXIMUM_AGE);
}

void file_sink_line(const char *line)
{
	if (log_stdout_enabled()) {
		(void)fprintf(stdout, "%s\n", line);
		(void)fflush(stdout);
	}
	if (log_file != NULL) {
		write_file_line(line);
		if (log_buffer_timeout_us == 0U)
			(void)fflush(log_file);
		if (log_maximum_size_bytes > 0U && log_size_bytes > log_maximum_size_bytes)
			rotate_log_file(ROTATION_MAXIMUM_SIZE);
	}
}

void file_flush(void)
{
	if (log_file != NULL)
		(void)fflush(log_file);
}

void file_close(void)
{
	if (log_file != NULL) {
		(void)fclose(log_file);
		log_file = NULL;
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
	log_opened_us = log_clock_us(CLOCK_MONOTONIC);
	log_last_flush_us = log_opened_us;
	log_maximum_age_us = settings->maximum_time_us;
	log_maximum_size_bytes = settings->maximum_size_bytes;
	log_size_bytes = (uint64_t)file_status.st_size;
	log_buffer_timeout_us = settings->buffer_timeout_us;
	log_compress_exports = settings->compress_exports;

	return 0;
}
