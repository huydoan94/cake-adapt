#define _XOPEN_SOURCE 700

#include "logging/log.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

/* A log file without rotation, buffering or compression. */
static const struct log_file_settings unlimited = { 0 };
static const struct log_file_settings size_limited = { .maximum_size_bytes = KILOBYTE };
static const struct log_file_settings size_limited_compressed = {
	.maximum_size_bytes = KILOBYTE,
	.compress_exports = true,
};
static const struct log_file_settings buffered = { .buffer_timeout_us = 500000U };
static const struct log_file_settings aged_and_buffered = {
	.maximum_time_us = MINUTE,
	.buffer_timeout_us = 500000U,
};
static const struct log_records every_record = {
	.data = true,
	.load = true,
	.reflector = true,
	.summary = true,
};
static const struct log_records load_records = { .load = true };
static const struct log_records tcp_queue_records = { .tcp_queue = true };
static const struct log_records memory_records = { .memory = true };
static bool use_mock_time;
static struct timespec mock_time;
static time_t mock_realtime_offset;
static unsigned int syslog_count;
static unsigned int clock_reads;
static int syslog_priority;
static char syslog_message[2048];
static bool fail_memstream_open;
static bool fail_memstream_close;
static FILE *fault_stream;
static unsigned int local_time_conversions;
/* Holds syslog calls, as a logd that stopped reading would. */
enum syslog_flow {
	SYSLOG_FLOWING,
	SYSLOG_HELD,
};

static atomic_int syslog_flow;
/* Counts syslog calls that found it held. */
static atomic_uint syslog_waits;
/* Counts syslog calls carrying the warning logged after a flood, and closelog() calls. */
static atomic_uint kept_warnings;
static atomic_uint closelog_calls;
/* The writer tests log as the service does: to syslog, not the terminal. */
static const bool in_foreground = false;

FILE *__real_open_memstream(char **buffer, size_t *size);
int __real_fclose(FILE *stream);
struct tm *__real_localtime_r(const time_t *sec, struct tm *result);

struct tm *__wrap_localtime_r(const time_t *sec, struct tm *result)
{
	local_time_conversions++;
	return __real_localtime_r(sec, result);
}

FILE *__wrap_open_memstream(char **buffer, size_t *size)
{
	FILE *stream;

	if (fail_memstream_open) {
		errno = ENOMEM;
		return NULL;
	}
	stream = __real_open_memstream(buffer, size);
	if (fail_memstream_close)
		fault_stream = stream;
	return stream;
}

int __wrap_fclose(FILE *stream)
{
	bool fail = stream == fault_stream;
	int result = __real_fclose(stream);

	if (fail) {
		fault_stream = NULL;
		errno = ENOMEM;
		return EOF;
	}
	return result;
}

static void capture_syslog(int priority, const char *format, va_list arguments)
{
	const struct timespec step = { .tv_nsec = 1000000L };

	if (atomic_load(&syslog_flow) == SYSLOG_HELD)
		atomic_fetch_add(&syslog_waits, 1U);
	while (atomic_load(&syslog_flow) == SYSLOG_HELD)
		(void)nanosleep(&step, NULL);
	syslog_count++;
	if (strstr(format, "%s") != NULL) {
		va_list copy;
		const char *text;

		va_copy(copy, arguments);
		text = va_arg(copy, const char *);
		if (strstr(text, "kept after flood") != NULL)
			atomic_fetch_add(&kept_warnings, 1U);
		va_end(copy);
	}
	syslog_priority = priority;
	(void)vsnprintf(syslog_message, sizeof(syslog_message), format, arguments);
}

void __wrap_syslog(int priority, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	capture_syslog(priority, format, arguments);
	va_end(arguments);
}

/* Counts the main thread's wakes of the log writer. */
static atomic_uint writer_wakes;

int __real_pthread_cond_signal(pthread_cond_t *condition);

int __wrap_pthread_cond_signal(pthread_cond_t *condition)
{
	atomic_fetch_add(&writer_wakes, 1U);
	return __real_pthread_cond_signal(condition);
}

void __real_closelog(void);

void __wrap_closelog(void)
{
	atomic_fetch_add(&closelog_calls, 1U);
	__real_closelog();
}

/* glibc fortification can route syslog through this checked entry point. */
void __wrap___syslog_chk(int priority, int flag, const char *format, ...)
{
	va_list arguments;

	(void)flag;
	va_start(arguments, format);
	capture_syslog(priority, format, arguments);
	va_end(arguments);
}

static void test_operational_syslog(void)
{
	log_init("cake-adapt-test", false);
	log_set_level(LOG_LEVEL_DEBUG);
	syslog_count = 0U;
	log_message(LOG_LEVEL_ERROR, "configuration failed");
	assert(syslog_count == 1U && syslog_priority == LOG_ERR);
	log_message(LOG_LEVEL_WARNING, "could not find interface '%s'", "wan");
	assert(syslog_count == 2U && syslog_priority == LOG_WARNING);
	assert(strstr(syslog_message, "could not find interface 'wan'") != NULL);
	log_message(LOG_LEVEL_INFO, "measurement status");
	log_message(LOG_LEVEL_DEBUG, "frequent sample");
	assert(syslog_count == 2U);
	log_system_message("disabled by configuration; exiting");
	assert(syslog_count == 3U);
	assert(strstr(syslog_message, "disabled by configuration") != NULL);
	log_set_debug_syslog(true);
	log_message(LOG_LEVEL_DEBUG, "opt-in debug");
	assert(syslog_count == 4U && syslog_priority == LOG_DEBUG);
	log_close();
}

/* Only helpers_log.o redirects the clock; production APIs remain unchanged. */
int test_log_clock_gettime(clockid_t clock_identifier, struct timespec *timestamp)
{
	clock_reads++;
	if (use_mock_time) {
		*timestamp = mock_time;
		if (clock_identifier == CLOCK_REALTIME)
			timestamp->tv_sec += mock_realtime_offset;
		return 0;
	}
	return clock_gettime(clock_identifier, timestamp);
}

/* Verbatim cake-autorate 3.3.0-PRERELEASE (ac75f493) headers. */
static const char expected_headers[] =
	"DATA_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
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
	" CAKE_UL_RATE_KBPS\n"
	"LOAD_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
	" DL_ACHIEVED_RATE_KBPS; UL_ACHIEVED_RATE_KBPS;"
	" CAKE_DL_RATE_KBPS; CAKE_UL_RATE_KBPS\n"
	"REFLECTOR_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US; REFLECTOR;"
	" MIN_SUM_OWD_BASELINES_US; SUM_OWD_BASELINES_US;"
	" SUM_OWD_BASELINES_DELTA_US; SUM_OWD_BASELINES_DELTA_THR_US;"
	" MIN_DL_DELTA_EWMA_US; DL_DELTA_EWMA_US; DL_DELTA_EWMA_DELTA_US;"
	" DL_DELTA_EWMA_DELTA_THR; MIN_UL_DELTA_EWMA_US; UL_DELTA_EWMA_US;"
	" UL_DELTA_EWMA_DELTA_US; UL_DELTA_EWMA_DELTA_THR\n"
	"SUMMARY_HEADER; LOG_DATETIME; LOG_TIMESTAMP; DL_ACHIEVED_RATE_KBPS;"
	" UL_ACHIEVED_RATE_KBPS; DL_SUM_DELAYS; UL_SUM_DELAYS;"
	" DL_AVG_OWD_DELTA_US; UL_AVG_OWD_DELTA_US; DL_LOAD_CONDITION;"
	" UL_LOAD_CONDITION; CAKE_DL_RATE_KBPS; CAKE_UL_RATE_KBPS\n";

static void read_log(const char *path, char *contents, size_t contents_size)
{
	FILE *file;
	size_t bytes_read;

	file = fopen(path, "r");
	assert(file != NULL);
	bytes_read = fread(contents, 1U, contents_size - 1U, file);
	assert(!ferror(file));
	contents[bytes_read] = '\0';
	assert(fclose(file) == 0);
}

/* A configuration check run by hand also prints warnings and errors to stderr. */
static void test_problems_to_stderr(void)
{
	char path[] = "/tmp/cake-adapt-stderr-XXXXXX";
	char contents[512];
	int descriptor = mkstemp(path);
	int saved = dup(STDERR_FILENO);

	assert(descriptor >= 0 && saved >= 0);
	assert(fflush(stderr) == 0 && dup2(descriptor, STDERR_FILENO) == STDERR_FILENO);
	log_init("cake-adapt-test", false);
	log_message(LOG_LEVEL_ERROR, "before the check");
	log_problems_to_stderr();
	syslog_count = 0U;
	log_message(LOG_LEVEL_ERROR, "configuration error: %s", "rates");
	log_message(LOG_LEVEL_WARNING, "interface overridden");
	log_message(LOG_LEVEL_NOTICE, "routine notice");
	assert(syslog_count == 2U);
	log_close();
	/* A new log_init() turns it off again. */
	log_init("cake-adapt-test", false);
	log_message(LOG_LEVEL_ERROR, "after the check");
	log_close();
	assert(fflush(stderr) == 0 && dup2(saved, STDERR_FILENO) == STDERR_FILENO);
	assert(close(saved) == 0 && close(descriptor) == 0);
	read_log(path, contents, sizeof(contents));
	assert(unlink(path) == 0);
	assert(strcmp(contents,
		      "ERROR: configuration error: rates\n"
		      "WARNING: interface overridden\n") == 0);
}

static void test_debug_logging_to_file(void)
{
	char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
	char contents[256];
	int descriptor;

	descriptor = mkstemp(path);
	assert(descriptor >= 0);
	assert(close(descriptor) == 0);

	log_init("sqm-mon-test", false);
	log_set_level(LOG_LEVEL_DEBUG);
	assert(log_set_file(path, &unlimited) == 0);
	log_message(LOG_LEVEL_DEBUG, "sample value=%d", 42);
	log_close();

	read_log(path, contents, sizeof(contents));
	assert(strstr(contents, "DEBUG; ") != NULL);
	assert(strstr(contents, "; sample value=42\n") != NULL);
	assert(unlink(path) == 0);
}

static void test_file_logging_respects_level(void)
{
	char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
	char contents[256];
	int descriptor;

	descriptor = mkstemp(path);
	assert(descriptor >= 0);
	assert(close(descriptor) == 0);

	log_init("sqm-mon-test", false);
	log_set_level(LOG_LEVEL_NOTICE);
	assert(log_set_file(path, &unlimited) == 0);
	log_message(LOG_LEVEL_DEBUG, "hidden debug message");
	log_message(LOG_LEVEL_NOTICE, "visible notice");
	log_close();

	read_log(path, contents, sizeof(contents));
	assert(strstr(contents, "INFO; ") != NULL);
	assert(strstr(contents, "; visible notice\n") != NULL);
	assert(strstr(contents, "hidden debug message") == NULL);
	assert(unlink(path) == 0);
}

static void test_empty_file_path_is_rejected(void)
{
	assert(log_set_file("", &unlimited) != 0);
}

static void test_failed_file_switch_preserves_rotation_path(void)
{
	char path[] = "/tmp/cake-adapt-log-test-XXXXXX";
	char invalid_path[128];
	char previous_path[128];
	char contents[2048];
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	log_init("cake-adapt-test", false);
	log_set_level(LOG_LEVEL_INFO);
	assert(log_set_file(path, &size_limited) == 0);
	(void)snprintf(invalid_path, sizeof(invalid_path), "%s/not-a-directory", path);
	assert(log_set_file(invalid_path, &size_limited) == -1);
	log_message(LOG_LEVEL_INFO, "%01100d", 42);
	log_close();
	(void)snprintf(previous_path, sizeof(previous_path), "%s.old", path);
	read_log(previous_path, contents, sizeof(contents));
	assert(strstr(contents, "42\n") != NULL);
	assert(unlink(previous_path) == 0);
	assert(unlink(path) == 0);
}

static void test_log_descriptor_is_close_on_exec(void)
{
	char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
	struct stat expected;
	struct dirent *entry;
	DIR *descriptors;
	bool found = false;
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(fstat(descriptor, &expected) == 0);
	assert(close(descriptor) == 0);
	log_init("sqm-mon-test", false);
	assert(log_set_file(path, &unlimited) == 0);
	descriptors = opendir("/proc/self/fd");
	assert(descriptors != NULL);
	while ((entry = readdir(descriptors)) != NULL) {
		struct stat current;
		char *end;
		long number = strtol(entry->d_name, &end, 10);

		if (*end != '\0')
			continue;
		descriptor = (int)number;
		if (fstat(descriptor, &current) == 0 && current.st_dev == expected.st_dev &&
		    current.st_ino == expected.st_ino) {
			int flags = fcntl(descriptor, F_GETFD);

			assert(flags >= 0 && (flags & FD_CLOEXEC) != 0);
			found = true;
		}
	}
	assert(closedir(descriptors) == 0);
	assert(found);
	log_close();
	assert(unlink(path) == 0);
}

static void assert_epoch_realtime_field(const char *field_start, const char *field_end)
{
	const char *character;
	const char *decimal_point = NULL;

	while (field_start < field_end && *field_start == ' ')
		field_start++;
	for (character = field_start; character < field_end; character++) {
		if (*character == '.') {
			assert(decimal_point == NULL);
			decimal_point = character;
		} else {
			assert(isdigit((unsigned char)*character) != 0);
		}
	}

	assert(decimal_point != NULL);
	assert(decimal_point > field_start);
	assert(field_end - decimal_point == 7);
}

static void assert_record_delimiter_count(const char *record, unsigned int expected_count)
{
	unsigned int delimiter_count = 0U;

	assert(record != NULL);
	while (*record != '\0' && *record != '\n') {
		if (*record == ';')
			delimiter_count++;
		record++;
	}
	assert(delimiter_count == expected_count);
}

static void test_cake_autorate_headers_and_record_format(void)
{
	char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
	char contents[8192];
	const struct log_data_record data_record = {
		.download_achieved_rate_bps = 10U * KILOBIT,
		.upload_achieved_rate_bps = 20U * KILOBIT,
		.download_load_ratio_e6 = 305000U,
		.upload_load_ratio_e6 = 400000U,
		.icmp_timestamp = "[50.00006]",
		.reflector = "1.1.1.1",
		.sequence = 70U,
		.download_owd_baseline_us = -80,
		.download_owd_us = -90,
		.download_owd_delta_ewma_us = -100,
		.download_owd_delta_us = -110,
		.download_adjust_delay_threshold_us = 120U,
		.upload_owd_baseline_us = -130,
		.upload_owd_us = -140,
		.upload_owd_delta_ewma_us = -150,
		.upload_owd_delta_us = -160,
		.upload_adjust_delay_threshold_us = 170U,
		.download_sum_delays = 180U,
		.download_average_owd_delta_us = -190,
		.download_maximum_adjust_up_threshold_us = 200U,
		.download_maximum_adjust_down_threshold_us = 210U,
		.upload_sum_delays = 220U,
		.upload_average_owd_delta_us = -230,
		.upload_maximum_adjust_up_threshold_us = 240U,
		.upload_maximum_adjust_down_threshold_us = 250U,
		.download_load_condition = "dl_low",
		.upload_load_condition = "ul_high_bb",
		.cake_download_rate_bps = 260U * KILOBIT,
		.cake_upload_rate_bps = 270U * KILOBIT,
	};
	const struct log_load_record load_record = {
		.download_achieved_rate_bps = 10U * KILOBIT,
		.upload_achieved_rate_bps = 20U * KILOBIT,
		.cake_download_rate_bps = 30U * KILOBIT,
		.cake_upload_rate_bps = 40U * KILOBIT,
	};
	const struct log_summary_record summary_record = {
		.download_achieved_rate_bps = 11U * KILOBIT,
		.upload_achieved_rate_bps = 21U * KILOBIT,
		.download_sum_delays = 31U,
		.upload_sum_delays = 41U,
		.download_average_owd_delta_us = -51,
		.upload_average_owd_delta_us = -61,
		.download_load_condition = "dl_idle",
		.upload_load_condition = "ul_low",
		.cake_download_rate_bps = 71U * KILOBIT,
		.cake_upload_rate_bps = 81U * KILOBIT,
	};
	const struct log_reflector_record reflector_record = {
		.reflector = "1.0.0.1",
		.minimum_sum_owd_baselines_us = -100,
		.sum_owd_baselines_us = -110,
		.sum_owd_baselines_delta_us = 10U,
		.sum_owd_baselines_delta_threshold_us = 20000U,
		.minimum_download_delta_ewma_us = -5,
		.download_delta_ewma_us = 7,
		.download_delta_ewma_delta_us = 12,
		.delta_ewma_delta_threshold_us = 10000U,
		.minimum_upload_delta_ewma_us = -6,
		.upload_delta_ewma_us = 8,
		.upload_delta_ewma_delta_us = 14,
	};
	const char *field_end;
	const char *field_start;
	const char *load_line;
	const char *record_line;
	int descriptor;

	descriptor = mkstemp(path);
	assert(descriptor >= 0);
	assert(close(descriptor) == 0);

	log_init("sqm-mon-test", false);
	assert(log_set_file(path, &unlimited) == 0);
	log_print_headers(&every_record);
	log_load(&load_record);
	log_data(&data_record);
	log_summary(&summary_record);
	log_reflector(&reflector_record);
	log_shaper("eth1", 28000U * KILOBIT);
	log_system_message("Started test process");
	log_close();

	read_log(path, contents, sizeof(contents));
	assert(strncmp(contents, expected_headers, strlen(expected_headers)) == 0);
	load_line = strstr(contents, "\nLOAD; ");
	assert(load_line != NULL);
	load_line++;
	assert_record_delimiter_count(load_line, 7U);

	record_line = strstr(contents, "\nDATA; ");
	assert(record_line != NULL);
	assert_record_delimiter_count(record_line + 1, 32U);
	record_line = strstr(contents, "\nSUMMARY; ");
	assert(record_line != NULL);
	assert_record_delimiter_count(record_line + 1, 12U);
	record_line = strstr(contents, "\nSHAPER; ");
	assert(record_line != NULL);
	assert_record_delimiter_count(record_line + 1, 3U);
	record_line = strstr(contents, "\nREFLECTOR; ");
	assert(record_line != NULL);
	assert_record_delimiter_count(record_line + 1, 16U);

	field_start = strchr(load_line, ';');
	assert(field_start != NULL);
	field_start = strchr(field_start + 1, ';');
	assert(field_start != NULL);
	field_end = strchr(field_start + 1, ';');
	assert(field_end != NULL);
	assert_epoch_realtime_field(field_start + 1, field_end);
	field_start = field_end;
	field_end = strchr(field_start + 1, ';');
	assert(field_end != NULL);
	assert_epoch_realtime_field(field_start + 1, field_end);
	assert(strstr(field_end, "; 10; 20; 30; 40\n") == field_end);
	assert(strstr(contents,
		      "; 10; 20; 30; 40; [50.00006]; 1.1.1.1; 70; -80; -90;"
		      " -100; -110; 120; -130; -140; -150; -160; 170; 180; -190;"
		      " 200; 210; 220; -230; 240; 250; dl_low; ul_high_bb; 260; 270\n") != NULL);
	assert(strstr(contents, "; 11; 21; 31; 41; -51; -61; dl_idle; ul_low; 71; 81\n") != NULL);
	assert(strstr(contents, "; tc qdisc change root dev eth1 cake bandwidth 28000Kbit\n") !=
	       NULL);
	assert(strstr(contents,
		      "; 1.0.0.1; -100; -110; 10; 20000; -5; 7; 12; 10000;"
		      " -6; 8; 14; 10000\n") != NULL);
	assert(strstr(contents, "SYSLOG; 20") != NULL);
	assert(strstr(contents, "; Started test process\n") != NULL);
	assert(unlink(path) == 0);
}

static void test_cpu_schema_matches_cake_autorate(void)
{
	char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
	char contents[4096];
	const struct cpu_sample sample = {
		.timestamp_us = 1234567U,
		.count = 2U,
		.counters = { { .identifier = "cpu",
				.user_ticks = 1U,
				.nice_ticks = 2U,
				.system_ticks = 3U,
				.idle_ticks = 4U,
				.iowait_ticks = 5U,
				.irq_ticks = 6U,
				.softirq_ticks = 7U,
				.steal_ticks = 8U,
				.guest_ticks = 9U,
				.guest_nice_ticks = 10U },
			      { .identifier = "cpu0", .idle_ticks = 50U } },
	};
	const struct cpu_busy usage[] = { { 40U, 100U }, { 1U, 2U } };
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	log_init("sqm-mon-test", false);
	assert(log_set_file(path, &unlimited) == 0);
	log_print_cpu_headers(&sample, true, true);
	log_cpu(&sample, usage);
	log_cpu_raw(&sample);
	log_close();
	read_log(path, contents, sizeof(contents));
	assert(strstr(
		       contents,
		       "CPU_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME; CPU_USAGE; CPU0_USAGE\n"
	       ) != NULL);
	assert(strstr(
		       contents,
		       "CPU_RAW_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME; CPU_ID; USER; NICE; SYSTEM; IDLE; IOWAIT; IRQ; SIRQ; STEAL; GUEST; GUEST_NICE\n"
	       ) != NULL);
	assert_record_delimiter_count(strstr(contents, "\nCPU; ") + 1, 5U);
	assert_record_delimiter_count(strstr(contents, "\nCPU_RAW; ") + 1, 14U);
	assert(strstr(contents, "; 1.234567; 40; 50\n") != NULL);
	assert(strstr(contents, "; 1.234567; cpu; 1; 2; 3; 4; 5; 6; 7; 8; 9; 10\n") != NULL);
	assert(unlink(path) == 0);
}

static void test_tcp_queue_record(void)
{
	char path[] = "/tmp/cake-adapt-log-test-XXXXXX";
	char contents[4096];
	const struct log_tcp_queue_record record = {
		.download_valid = true,
		.upload_valid = false,
		.download_queue_us = 1500,
		.upload_queue_us = -20,
	};
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	log_init("cake-adapt-test", false);
	assert(log_set_file(path, &unlimited) == 0);
	log_print_headers(&tcp_queue_records);
	log_tcp_queue(&record);
	log_close();
	read_log(path, contents, sizeof(contents));
	assert(strstr(contents,
		      "TCP_QUEUE_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
		      " DL_QUEUE_VALID; DL_QUEUE_US; UL_QUEUE_VALID; UL_QUEUE_US\nTCP_QUEUE; ") ==
	       contents);
	assert_record_delimiter_count(strstr(contents, "\nTCP_QUEUE; ") + 1, 7U);
	assert(strstr(contents, "; 1; 1500; 0; -20\n") != NULL);
	assert(unlink(path) == 0);
}

static void test_memory_record(void)
{
	char path[] = "/tmp/cake-adapt-log-test-XXXXXX";
	char contents[4096];
	const struct memory_sample sample = {
		.rss_bytes = 2136U * KILOBYTE,
		.peak_rss_bytes = 2200U * KILOBYTE,
		.anonymous_bytes = 412U * KILOBYTE,
		.data_bytes = 900U * KILOBYTE,
	};
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	log_init("cake-adapt-test", false);
	assert(log_set_file(path, &unlimited) == 0);
	log_print_headers(&memory_records);
	log_memory(&sample);
	log_close();
	read_log(path, contents, sizeof(contents));
	assert(strstr(contents,
		      "MEMORY_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
		      " RSS_KB; PEAK_RSS_KB; RSS_ANON_KB; DATA_KB\nMEMORY; ") == contents);
	assert_record_delimiter_count(strstr(contents, "\nMEMORY; ") + 1, 7U);
	assert(strstr(contents, "; 2136; 2200; 412; 900\n") != NULL);
	assert(unlink(path) == 0);
}

static void
test_rotation_export_and_reset_preserve_live_inode(const struct log_file_settings *settings)
{
	bool compress = settings->compress_exports;
	char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
	char previous_path[128];
	char export_path[256];
	char contents[4096];
	char large_message[1600];
	struct stat before;
	struct stat after;
	gzFile export_file;
	int length;
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	assert(stat(path, &before) == 0);
	memset(large_message, 'x', sizeof(large_message) - 1U);
	large_message[sizeof(large_message) - 1U] = '\0';
	memcpy(large_message, "before rotation ", 16U);
	log_init("sqm-mon-test", false);
	log_set_level(LOG_LEVEL_INFO);
	assert(log_set_file(path, settings) == 0);
	log_print_headers(&load_records);
	log_message(LOG_LEVEL_INFO, "%s", large_message);
	assert(stat(path, &after) == 0);
	assert(before.st_ino == after.st_ino);
	(void)snprintf(previous_path, sizeof(previous_path), "%s.old", path);
	read_log(previous_path, contents, sizeof(contents));
	assert(strstr(contents, "before rotation ") != NULL);
	log_message(LOG_LEVEL_INFO, "after rotation");
	assert(log_export_file(export_path, sizeof(export_path)) == 0);
	assert((strcmp(export_path + strlen(export_path) - 3U, ".gz") == 0) == compress);
	export_file = gzopen(export_path, "rb");
	assert(export_file != NULL);
	assert(gzdirect(export_file) == (compress ? 0 : 1));
	length = gzread(export_file, contents, (unsigned int)(sizeof(contents) - 1U));
	assert(length > 0);
	contents[length] = '\0';
	assert(gzclose(export_file) == Z_OK);
	assert(strstr(contents, "before rotation ") != NULL);
	assert(strstr(contents, "after rotation") != NULL);
	assert(log_reset_file() == 0);
	assert(stat(path, &after) == 0);
	assert(before.st_ino == after.st_ino);
	log_close();
	read_log(path, contents, sizeof(contents));
	assert(strncmp(contents, "LOAD_HEADER; ", 13U) == 0);
	assert(strstr(contents, "after rotation") == NULL);
	assert(unlink(path) == 0);
	assert(unlink(previous_path) == 0);
	assert(unlink(export_path) == 0);
}

static void test_cpu_log_allocation_failures(void)
{
	char path[] = "/tmp/cake-adapt-log-memory-XXXXXX";
	char contents[4096];
	struct cpu_sample sample = { .count = 1U, .counters = { { .identifier = "cpu" } } };
	const struct cpu_busy usage[] = { { 40U, 100U } };
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	log_init("cake-adapt-test", false);
	assert(log_set_file(path, &unlimited) == 0);
	syslog_count = 0U;
	fail_memstream_open = true;
	log_print_cpu_headers(&sample, true, true);
	log_cpu(&sample, usage);
	fail_memstream_open = false;
	fail_memstream_close = true;
	log_print_cpu_headers(&sample, true, true);
	log_cpu(&sample, usage);
	fail_memstream_close = false;
	assert(syslog_count == 4U && syslog_priority == LOG_WARNING);
	read_log(path, contents, sizeof(contents));
	assert(strstr(contents, "CPU_RAW_HEADER;") != NULL);
	assert(strstr(contents, "CPU_HEADER;") == NULL);
	assert(strstr(contents, "\nCPU;") == NULL);

	log_print_cpu_headers(&sample, true, true);
	log_cpu(&sample, usage);
	log_close();
	read_log(path, contents, sizeof(contents));
	assert(strstr(contents, "CPU_HEADER;") != NULL);
	assert(strstr(contents, "\nCPU;") != NULL);
	assert(unlink(path) == 0);
}

static void test_buffer_timeout_and_time_rotation(void)
{
	char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
	char previous_path[128];
	char contents[4096];
	struct stat before;
	struct stat after;
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	assert(stat(path, &before) == 0);
	use_mock_time = true;
	mock_time = (struct timespec){ .tv_sec = 1000 };
	log_init("sqm-mon-test", false);
	assert(log_set_file(path, &aged_and_buffered) == 0);
	log_print_headers(&load_records);
	log_message(LOG_LEVEL_INFO, "buffered until timeout");
	read_log(path, contents, sizeof(contents));
	assert(contents[0] == '\0');
	mock_time.tv_nsec = 499999000L;
	log_tick();
	read_log(path, contents, sizeof(contents));
	assert(contents[0] == '\0');
	mock_time.tv_nsec = 500000000L;
	log_tick();
	read_log(path, contents, sizeof(contents));
	assert(strstr(contents, "buffered until timeout") != NULL);
	mock_realtime_offset = 3600;
	log_tick();
	read_log(path, contents, sizeof(contents));
	assert(strstr(contents, "buffered until timeout") != NULL);
	mock_realtime_offset = 0;
	mock_time.tv_sec = 1060;
	mock_time.tv_nsec = 0;
	log_tick();
	read_log(path, contents, sizeof(contents));
	assert(strstr(contents, "buffered until timeout") != NULL);
	mock_time.tv_sec++;
	log_tick();
	read_log(path, contents, sizeof(contents));
	assert(strstr(contents, "buffered until timeout") == NULL);
	assert(stat(path, &after) == 0);
	assert(before.st_ino == after.st_ino);
	(void)snprintf(previous_path, sizeof(previous_path), "%s.old", path);
	read_log(previous_path, contents, sizeof(contents));
	assert(strstr(contents, "buffered until timeout") != NULL);
	log_close();
	use_mock_time = false;
	assert(unlink(path) == 0);
	assert(unlink(previous_path) == 0);
}

static void test_local_time_is_converted_once_per_sec(void)
{
	char path[] = "/tmp/cake-adapt-log-datetime-XXXXXX";
	char contents[4096];
	char expected[64];
	struct tm local_time;
	time_t sec = 3000;
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	use_mock_time = true;
	mock_time = (struct timespec){ .tv_sec = sec };
	log_init("cake-adapt-test", false);
	log_set_level(LOG_LEVEL_INFO);
	assert(log_set_file(path, &unlimited) == 0);
	local_time_conversions = 0U;
	log_message(LOG_LEVEL_INFO, "first");
	mock_time.tv_nsec = 999999000L;
	log_message(LOG_LEVEL_INFO, "same second");
	assert(local_time_conversions == 1U);
	mock_time.tv_sec++;
	mock_time.tv_nsec = 0L;
	log_message(LOG_LEVEL_INFO, "next second");
	assert(local_time_conversions == 2U);

	read_log(path, contents, sizeof(contents));
	assert(localtime_r(&sec, &local_time) != NULL);
	assert(strftime(
		       expected,
		       sizeof(expected),
		       "INFO; %Y-%m-%d-%H:%M:%S; 3000.999999; same second",
		       &local_time
	       ) > 0U);
	assert(strstr(contents, expected) != NULL);
	sec++;
	assert(localtime_r(&sec, &local_time) != NULL);
	assert(strftime(
		       expected,
		       sizeof(expected),
		       "INFO; %Y-%m-%d-%H:%M:%S; 3001.000000; next second",
		       &local_time
	       ) > 0U);
	assert(strstr(contents, expected) != NULL);
	log_close();
	use_mock_time = false;
	assert(unlink(path) == 0);
}

static void test_records_wait_for_buffer_timeout(void)
{
	char path[] = "/tmp/cake-adapt-log-buffer-XXXXXX";
	char contents[16384];
	char record[512];
	struct stat status;
	size_t index;
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	use_mock_time = true;
	mock_time = (struct timespec){ .tv_sec = 4000 };
	log_init("cake-adapt-test", false);
	log_set_level(LOG_LEVEL_INFO);
	assert(log_set_file(path, &buffered) == 0);
	memset(record, 'x', sizeof(record) - 1U);
	record[sizeof(record) - 1U] = '\0';
	/* More than libc's default stream buffer, but one timer period of records. */
	for (index = 0U; index < 16U; index++)
		log_message(LOG_LEVEL_INFO, "%s", record);
	assert(stat(path, &status) == 0);
	assert(status.st_size == 0);
	mock_time.tv_nsec = 500000000L;
	log_tick();
	read_log(path, contents, sizeof(contents));
	assert(strlen(contents) > 16U * (sizeof(record) - 1U));
	log_close();
	use_mock_time = false;
	assert(unlink(path) == 0);
}

static void test_immediate_output_avoids_maintenance_clock(void)
{
	char path[] = "/tmp/cake-adapt-log-immediate-XXXXXX";
	char contents[1024];
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	use_mock_time = true;
	mock_time = (struct timespec){ .tv_sec = 2000 };
	log_init("cake-adapt-test", false);
	assert(log_set_file(path, &unlimited) == 0);
	clock_reads = 0U;
	log_print_headers(&load_records);
	assert(clock_reads == 0U);
	read_log(path, contents, sizeof(contents));
	assert(strncmp(contents, "LOAD_HEADER; ", 13U) == 0);
	log_close();
	use_mock_time = false;
	assert(unlink(path) == 0);
}

static void test_existing_file_size_uses_strict_rotation_limit(void)
{
	char path[] = "/tmp/cake-adapt-log-size-XXXXXX";
	char previous_path[128];
	char contents[2048];
	char existing[KILOBYTE];
	FILE *file;
	int descriptor = mkstemp(path);

	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	memset(existing, 'x', sizeof(existing));
	file = fopen(path, "w");
	assert(file != NULL);
	assert(fwrite(existing, 1U, sizeof(existing), file) == sizeof(existing));
	assert(fclose(file) == 0);

	log_init("cake-adapt-test", false);
	log_set_level(LOG_LEVEL_INFO);
	assert(log_set_file(path, &size_limited) == 0);
	log_tick();
	(void)snprintf(previous_path, sizeof(previous_path), "%s.old", path);
	assert(access(previous_path, F_OK) != 0 && errno == ENOENT);
	log_message(LOG_LEVEL_INFO, "cross size threshold");
	read_log(previous_path, contents, sizeof(contents));
	assert(strstr(contents, "cross size threshold") != NULL);
	log_close();
	assert(unlink(previous_path) == 0);
	assert(unlink(path) == 0);
}

static void test_disabled_output_skips_formatting_clocks(void)
{
	const struct log_load_record load = { 0 };

	log_init("cake-adapt-test", false);
	log_set_level(LOG_LEVEL_DEBUG);
	clock_reads = 0U;
	syslog_count = 0U;
	log_load(&load);
	log_shaper("wan", 1000U * KILOBIT);
	log_message(LOG_LEVEL_DEBUG, "unused debug sample");
	log_message(LOG_LEVEL_INFO, "unused information");
	log_tick();
	assert(clock_reads == 0U);
	assert(syslog_count == 0U);

	log_message(LOG_LEVEL_WARNING, "operational warning remains visible");
	assert(clock_reads == 1U);
	assert(syslog_count == 1U);
	log_close();
}

extern bool test_mqtt_active;
extern unsigned int test_mqtt_count;
extern char test_mqtt_line[2048];

static void test_mqtt_without_file(void)
{
	bool foreground = false;

	log_init("test", foreground);
	test_mqtt_active = true;
	test_mqtt_count = 0U;
	log_message(LOG_LEVEL_INFO, "MQTT-only message");
	assert(test_mqtt_count == 1U);
	assert(strstr(test_mqtt_line, "INFO;") != NULL);
	assert(strstr(test_mqtt_line, "; MQTT-only message") != NULL);
	log_system_message("MQTT system message");
	assert(test_mqtt_count == 2U);
	assert(strstr(test_mqtt_line, "SYSLOG;") != NULL);
	log_print_headers(&(struct log_records){ .load = true });
	assert(test_mqtt_count == 3U);
	assert(strncmp(test_mqtt_line, "LOAD_HEADER;", 12U) == 0);
	/* Records reach MQTT without a log file too, timed or not. */
	log_load(&(struct log_load_record){ .download_achieved_rate_bps = 1000U });
	assert(test_mqtt_count == 4U);
	assert(strncmp(test_mqtt_line, "LOAD;", 5U) == 0);
	log_shaper("eth0", 2000000U);
	assert(test_mqtt_count == 5U);
	assert(strncmp(test_mqtt_line, "SHAPER;", 7U) == 0);
	log_close();
	assert(!test_mqtt_active);
}

static uint64_t real_monotonic_us(void)
{
	struct timespec now;

	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * MICROSECONDS_PER_SECOND + (uint64_t)now.tv_nsec / 1000U;
}

/* The whole log file, NUL-terminated, in a buffer the caller frees. */
static char *read_whole(const char *path)
{
	FILE *file = fopen(path, "r");
	char *text;
	long size;

	assert(file != NULL);
	assert(fseek(file, 0, SEEK_END) == 0);
	size = ftell(file);
	assert(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
	text = malloc((size_t)size + 1U);
	assert(text != NULL);
	assert(fread(text, 1U, (size_t)size, file) == (size_t)size);
	text[size] = '\0';
	(void)__real_fclose(file);
	return text;
}

static void start_writer_on(const char *path)
{
	char error[256] = "";

	use_mock_time = false;
	log_init("cake-adapt-test", in_foreground);
	log_set_level(LOG_LEVEL_INFO);
	assert(log_set_file(path, &buffered) == 0);
	assert(log_start_writer(error, sizeof(error)) == 0);
}

/* The writer writes every line the main thread queued, in order. */
static void test_writer_keeps_order(void)
{
	char path[] = "/tmp/cake-adapt-writer-XXXXXX";
	int descriptor = mkstemp(path);
	const char *position;
	char *text;
	char expected[64];

	assert(descriptor >= 0);
	(void)close(descriptor);
	start_writer_on(path);
	for (unsigned int line = 0U; line < 1000U; line++)
		log_message(LOG_LEVEL_INFO, "ordered %u", line);
	assert(log_close());
	text = read_whole(path);
	position = text;
	for (unsigned int line = 0U; line < 1000U; line++) {
		(void)snprintf(expected, sizeof(expected), "; ordered %u\n", line);
		position = strstr(position, expected);
		assert(position != NULL);
	}
	free(text);
	assert(unlink(path) == 0);
}

/*
 * Lines wait for the writer's timer and still reach the file within the
 * buffer timeout; a syslog entry wakes the writer at once.
 */
static void test_lines_wait_for_writer_timer(void)
{
	char path[] = "/tmp/cake-adapt-writer-XXXXXX";
	int descriptor = mkstemp(path);
	const struct timespec step = { .tv_nsec = 10000000L };
	unsigned int wakes;
	uint64_t start_us;

	assert(descriptor >= 0);
	(void)close(descriptor);
	start_writer_on(path);
	wakes = atomic_load(&writer_wakes);
	for (unsigned int line = 0U; line < 100U; line++)
		log_message(LOG_LEVEL_INFO, "quiet %u", line);
	assert(atomic_load(&writer_wakes) == wakes);
	start_us = real_monotonic_us();
	for (;;) {
		char *text = read_whole(path);
		bool written = strstr(text, "; quiet 99\n") != NULL;

		free(text);
		if (written)
			break;
		/* The 500-ms buffer timeout, with room for a slow test run. */
		assert(real_monotonic_us() - start_us < 2U * SECOND);
		(void)nanosleep(&step, NULL);
	}
	log_message(LOG_LEVEL_WARNING, "wakes the writer");
	assert(atomic_load(&writer_wakes) == wakes + 1U);
	assert(log_close());
	assert(unlink(path) == 0);
}

/* Without a buffer timeout every line is written at once, so each wakes the writer. */
static void test_unbuffered_lines_wake_writer(void)
{
	char path[] = "/tmp/cake-adapt-writer-XXXXXX";
	int descriptor = mkstemp(path);
	char error[256] = "";
	unsigned int wakes;

	assert(descriptor >= 0);
	(void)close(descriptor);
	log_init("cake-adapt-test", in_foreground);
	log_set_level(LOG_LEVEL_INFO);
	assert(log_set_file(path, &unlimited) == 0);
	assert(log_start_writer(error, sizeof(error)) == 0);
	wakes = atomic_load(&writer_wakes);
	for (unsigned int line = 0U; line < 10U; line++)
		log_message(LOG_LEVEL_INFO, "at once %u", line);
	assert(atomic_load(&writer_wakes) == wakes + 10U);
	assert(log_close());
	assert(unlink(path) == 0);
}

/* A writer behind on a half-full queue is woken by every further line. */
static void test_half_full_queue_wakes_writer(void)
{
	char path[] = "/tmp/cake-adapt-writer-XXXXXX";
	int descriptor = mkstemp(path);
	unsigned int waits;
	unsigned int wakes;
	uint64_t start_us;

	assert(descriptor >= 0);
	(void)close(descriptor);
	waits = atomic_load(&syslog_waits);
	atomic_store(&syslog_flow, SYSLOG_HELD);
	start_writer_on(path);
	log_message(LOG_LEVEL_WARNING, "hold the writer");
	start_us = real_monotonic_us();
	while (atomic_load(&syslog_waits) == waits)
		assert(real_monotonic_us() - start_us < SECOND);
	wakes = atomic_load(&writer_wakes);
	/* About 40 KB, under half the 128-KiB queue. */
	for (unsigned int line = 0U; line < 400U; line++)
		log_message(
			LOG_LEVEL_INFO,
			"filling %u ...................................................",
			line
		);
	assert(atomic_load(&writer_wakes) == wakes);
	/* About 40 KB more, past half. */
	for (unsigned int line = 0U; line < 400U; line++)
		log_message(
			LOG_LEVEL_INFO,
			"filling %u ...................................................",
			line
		);
	assert(atomic_load(&writer_wakes) > wakes);
	atomic_store(&syslog_flow, SYSLOG_FLOWING);
	assert(log_close());
	assert(unlink(path) == 0);
}

/*
 * A stuck syslog holds only the writer: the main thread keeps logging at full
 * speed. When the queue is full the oldest lines make room, so the newest are
 * kept; the writer says how many were overwritten, before the oldest line it
 * still had, and kept plus overwritten account for every line.
 */
static void test_stuck_syslog_does_not_block(void)
{
	char path[] = "/tmp/cake-adapt-writer-XXXXXX";
	int descriptor = mkstemp(path);
	unsigned int waits;
	uint64_t start_us;
	char *text;
	const char *note_text;
	const char *line;
	unsigned long long overwritten = 0ULL;
	unsigned long long kept = 0ULL;

	assert(descriptor >= 0);
	(void)close(descriptor);
	waits = atomic_load(&syslog_waits);
	atomic_store(&syslog_flow, SYSLOG_HELD);
	start_writer_on(path);
	/* Its syslog entry holds the writer; its file line waits in the queue. */
	log_message(LOG_LEVEL_WARNING, "first warning");
	start_us = real_monotonic_us();
	while (atomic_load(&syslog_waits) == waits)
		assert(real_monotonic_us() - start_us < SECOND);
	/* About 1 MB, eight times the queue. */
	start_us = real_monotonic_us();
	for (unsigned int index = 0U; index < 8000U; index++)
		log_message(
			LOG_LEVEL_INFO,
			"flood %u ................................................................................",
			index
		);
	log_message(LOG_LEVEL_WARNING, "kept after flood");
	/* Loose for slow test runs: a queue that blocked would never get here. */
	assert(real_monotonic_us() - start_us < 2U * SECOND);
	atomic_store(&syslog_flow, SYSLOG_FLOWING);
	assert(log_close());
	assert(atomic_load(&kept_warnings) == 1U);
	text = read_whole(path);
	/* The newest survive; the oldest made room for them. */
	assert(strstr(text, "; flood 7999 ") != NULL && strstr(text, "; kept after flood") != NULL);
	assert(strstr(text, "; flood 0 ") == NULL && strstr(text, "; first warning") == NULL);
	note_text = strstr(text, " older log lines and 0 syslog messages were overwritten");
	assert(note_text != NULL);
	/* Reported before the oldest line still queued: no flood line comes earlier. */
	line = strstr(text, "; flood ");
	assert(line != NULL && line > note_text);
	while (note_text > text && isdigit((unsigned char)note_text[-1]))
		note_text--;
	assert(sscanf(note_text, "%llu", &overwritten) == 1);
	for (line = strstr(text, "; flood "); line != NULL; line = strstr(line + 1, "; flood "))
		kept++;
	/* Every flood line, and the first warning's file line, either kept or counted. */
	assert(kept + overwritten == 8001ULL);
	free(text);
	assert(unlink(path) == 0);
}

/* An export request that the flood overwrote is still carried out. */
static void test_overwritten_export_still_runs(void)
{
	char directory[] = "/tmp/cake-adapt-writer-XXXXXX";
	char path[256];
	DIR *listing;
	const struct dirent *entry;
	unsigned int exports = 0U;
	unsigned int waits;
	uint64_t start_us;

	assert(mkdtemp(directory) != NULL);
	(void)snprintf(path, sizeof(path), "%s/cake-adapt.log", directory);
	waits = atomic_load(&syslog_waits);
	atomic_store(&syslog_flow, SYSLOG_HELD);
	start_writer_on(path);
	log_message(LOG_LEVEL_WARNING, "hold the writer");
	start_us = real_monotonic_us();
	while (atomic_load(&syslog_waits) == waits)
		assert(real_monotonic_us() - start_us < SECOND);
	log_request_export();
	for (unsigned int index = 0U; index < 8000U; index++)
		log_message(
			LOG_LEVEL_INFO,
			"flood %u ................................................................................",
			index
		);
	atomic_store(&syslog_flow, SYSLOG_FLOWING);
	assert(log_close());
	listing = opendir(directory);
	assert(listing != NULL);
	while ((entry = readdir(listing)) != NULL) {
		char name[512];

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (strncmp(entry->d_name, "cake-adapt_", 11U) == 0)
			exports++;
		(void)snprintf(name, sizeof(name), "%s/%s", directory, entry->d_name);
		assert(unlink(name) == 0);
	}
	(void)closedir(listing);
	assert(exports == 1U);
	assert(rmdir(directory) == 0);
}

/* Export and reset requests run on the writer, in order with the lines around them. */
static void test_writer_export_and_reset_in_order(void)
{
	char directory[] = "/tmp/cake-adapt-writer-XXXXXX";
	char path[256];
	DIR *listing;
	const struct dirent *entry;
	bool exported = false;
	char *text;

	assert(mkdtemp(directory) != NULL);
	(void)snprintf(path, sizeof(path), "%s/cake-adapt.log", directory);
	start_writer_on(path);
	log_message(LOG_LEVEL_INFO, "before reset");
	log_request_export();
	log_request_reset();
	log_message(LOG_LEVEL_INFO, "after reset");
	assert(log_close());
	text = read_whole(path);
	assert(strstr(text, "before reset") == NULL && strstr(text, "after reset") != NULL);
	free(text);
	listing = opendir(directory);
	assert(listing != NULL);
	while ((entry = readdir(listing)) != NULL) {
		char name[512];

		if (strncmp(entry->d_name, "cake-adapt_", 11U) != 0)
			continue;
		exported = true;
		(void)snprintf(name, sizeof(name), "%s/%s", directory, entry->d_name);
		assert(unlink(name) == 0);
	}
	(void)closedir(listing);
	assert(exported);
	(void)snprintf(path, sizeof(path), "%s/cake-adapt.log", directory);
	assert(unlink(path) == 0);
	(void)snprintf(path, sizeof(path), "%s/cake-adapt.log.old", directory);
	(void)unlink(path);
	assert(rmdir(directory) == 0);
}

/* Stopping waits for a stuck writer only so long, then leaves it behind. Run last. */
static void test_stuck_writer_stop_is_bounded(void)
{
	char path[] = "/tmp/cake-adapt-writer-XXXXXX";
	int descriptor = mkstemp(path);
	const struct timespec settle = { .tv_nsec = 50000000L };
	unsigned int waits;
	unsigned int calls;
	uint64_t start_us;

	assert(descriptor >= 0);
	(void)close(descriptor);
	waits = atomic_load(&syslog_waits);
	atomic_store(&syslog_flow, SYSLOG_HELD);
	start_writer_on(path);
	log_message(LOG_LEVEL_WARNING, "stuck at stop");
	start_us = real_monotonic_us();
	while (atomic_load(&syslog_waits) == waits)
		assert(real_monotonic_us() - start_us < SECOND);
	start_us = real_monotonic_us();
	calls = atomic_load(&closelog_calls);
	/* Left behind: no closelog() or fclose() that would wait for the stuck writer. */
	assert(!log_close());
	assert(real_monotonic_us() - start_us < 3U * SECOND);
	assert(atomic_load(&closelog_calls) == calls);
	atomic_store(&syslog_flow, SYSLOG_FLOWING);
	(void)nanosleep(&settle, NULL);
	assert(unlink(path) == 0);
}

int main(void)
{
	test_mqtt_without_file();
	test_operational_syslog();
	test_problems_to_stderr();
	test_disabled_output_skips_formatting_clocks();
	test_failed_file_switch_preserves_rotation_path();
	test_debug_logging_to_file();
	test_file_logging_respects_level();
	test_empty_file_path_is_rejected();
	test_log_descriptor_is_close_on_exec();
	test_cake_autorate_headers_and_record_format();
	test_cpu_schema_matches_cake_autorate();
	test_cpu_log_allocation_failures();
	test_tcp_queue_record();
	test_memory_record();
	test_rotation_export_and_reset_preserve_live_inode(&size_limited);
	test_rotation_export_and_reset_preserve_live_inode(&size_limited_compressed);
	test_buffer_timeout_and_time_rotation();
	test_local_time_is_converted_once_per_sec();
	test_records_wait_for_buffer_timeout();
	test_immediate_output_avoids_maintenance_clock();
	test_existing_file_size_uses_strict_rotation_limit();
	/* A writer that blocks its caller would hang these tests: fail them instead. */
	(void)alarm(60U);
	test_writer_keeps_order();
	test_lines_wait_for_writer_timer();
	test_unbuffered_lines_wake_writer();
	test_half_full_queue_wakes_writer();
	test_stuck_syslog_does_not_block();
	test_overwritten_export_still_runs();
	test_writer_export_and_reset_in_order();
	test_stuck_writer_stop_is_bounded();
	(void)alarm(0U);

	(void)puts("log tests passed");
	return 0;
}
