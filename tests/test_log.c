#define _XOPEN_SOURCE 700

#include "log.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static bool use_mock_time;
static struct timespec mock_time;
static time_t mock_realtime_offset;
static unsigned int syslog_count;
static int syslog_priority;
static char syslog_message[2048];

static void capture_syslog(int priority, const char *format, va_list arguments)
{
    syslog_count++;
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
    assert(log_set_level("debug") == 0);
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
int test_log_clock_gettime(
    clockid_t clock_identifier,
    struct timespec *timestamp
)
{
    if (use_mock_time) {
        *timestamp = mock_time;
        if (clock_identifier == CLOCK_REALTIME) {
            timestamp->tv_sec += mock_realtime_offset;
        }
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

static void read_log(
    const char *path,
    char *contents,
    size_t contents_size
)
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

static void test_debug_logging_to_file(void)
{
    char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
    char contents[256];
    int descriptor;

    descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);

    log_init("sqm-mon-test", false);
    assert(log_set_level("debug") == 0);
    assert(log_set_file(path, 0U, 0U, 0U, false) == 0);
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
    assert(log_set_level("notice") == 0);
    assert(log_set_file(path, 0U, 0U, 0U, false) == 0);
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
    assert(log_set_file("", 0U, 0U, 0U, false) != 0);
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
    assert(log_set_level("info") == 0);
    assert(log_set_file(path, 0U, 1U, 0U, false) == 0);
    (void)snprintf(invalid_path, sizeof(invalid_path), "%s/not-a-directory", path);
    assert(log_set_file(invalid_path, 0U, 1U, 0U, false) == -1);
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
    assert(log_set_file(path, 0U, 0U, 0U, false) == 0);
    descriptors = opendir("/proc/self/fd");
    assert(descriptors != NULL);
    while ((entry = readdir(descriptors)) != NULL) {
        struct stat current;
        char *end;
        long number = strtol(entry->d_name, &end, 10);

        if (*end != '\0') {
            continue;
        }
        descriptor = (int)number;
        if (fstat(descriptor, &current) == 0 &&
            current.st_dev == expected.st_dev && current.st_ino == expected.st_ino) {
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

static void assert_epoch_realtime_field(
    const char *field_start,
    const char *field_end
)
{
    const char *character;
    const char *decimal_point = NULL;

    while (field_start < field_end && *field_start == ' ') {
        field_start++;
    }
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

static void assert_record_delimiter_count(
    const char *record,
    unsigned int expected_count
)
{
    unsigned int delimiter_count = 0U;

    assert(record != NULL);
    while (*record != '\0' && *record != '\n') {
        if (*record == ';') {
            delimiter_count++;
        }
        record++;
    }
    assert(delimiter_count == expected_count);
}

static void test_cake_autorate_headers_and_record_format(void)
{
    char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
    char contents[8192];
    const struct log_data_record data_record = {
        .download_achieved_rate_kbps = 10U,
        .upload_achieved_rate_kbps = 20U,
        .download_load_percent = 30U,
        .upload_load_percent = 40U,
        .icmp_timestamp_microseconds = 50000060U,
        .reflector = "1.1.1.1",
        .sequence = 70U,
        .download_owd_baseline_microseconds = 80U,
        .download_owd_microseconds = 90U,
        .download_owd_delta_ewma_microseconds = -100,
        .download_owd_delta_microseconds = -110,
        .download_adjust_delay_threshold_microseconds = 120U,
        .upload_owd_baseline_microseconds = 130U,
        .upload_owd_microseconds = 140U,
        .upload_owd_delta_ewma_microseconds = -150,
        .upload_owd_delta_microseconds = -160,
        .upload_adjust_delay_threshold_microseconds = 170U,
        .download_sum_delays = 180U,
        .download_average_owd_delta_microseconds = -190,
        .download_maximum_adjust_up_threshold_microseconds = 200U,
        .download_maximum_adjust_down_threshold_microseconds = 210U,
        .upload_sum_delays = 220U,
        .upload_average_owd_delta_microseconds = -230,
        .upload_maximum_adjust_up_threshold_microseconds = 240U,
        .upload_maximum_adjust_down_threshold_microseconds = 250U,
        .download_load_condition = "dl_low",
        .upload_load_condition = "ul_high_bb",
        .cake_download_rate_kbps = 260U,
        .cake_upload_rate_kbps = 270U
    };
    const struct log_load_record load_record = {
        .download_achieved_rate_kbps = 10U,
        .upload_achieved_rate_kbps = 20U,
        .cake_download_rate_kbps = 30U,
        .cake_upload_rate_kbps = 40U
    };
    const struct log_summary_record summary_record = {
        .download_achieved_rate_kbps = 11U,
        .upload_achieved_rate_kbps = 21U,
        .download_sum_delays = 31U,
        .upload_sum_delays = 41U,
        .download_average_owd_delta_microseconds = -51,
        .upload_average_owd_delta_microseconds = -61,
        .download_load_condition = "dl_idle",
        .upload_load_condition = "ul_low",
        .cake_download_rate_kbps = 71U,
        .cake_upload_rate_kbps = 81U
    };
    const struct log_reflector_record reflector_record = {
        .reflector = "1.0.0.1",
        .minimum_sum_owd_baselines_microseconds = 100U,
        .sum_owd_baselines_microseconds = 110U,
        .sum_owd_baselines_delta_microseconds = 10U,
        .sum_owd_baselines_delta_threshold_microseconds = 20000U,
        .minimum_download_delta_ewma_microseconds = -5,
        .download_delta_ewma_microseconds = 7,
        .download_delta_ewma_delta_microseconds = 12,
        .delta_ewma_delta_threshold_microseconds = 10000U,
        .minimum_upload_delta_ewma_microseconds = -6,
        .upload_delta_ewma_microseconds = 8,
        .upload_delta_ewma_delta_microseconds = 14
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
    assert(log_set_file(path, 0U, 0U, 0U, false) == 0);
    log_print_headers(true, true, true, true);
    log_load(&load_record);
    log_data(&data_record);
    log_summary(&summary_record);
    log_reflector(&reflector_record);
    log_shaper("eth1", 28000U);
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
    assert(strstr(
        contents,
        "; 10; 20; 30; 40; 50.000060; 1.1.1.1; 70; 80; 90;"
        " -100; -110; 120; 130; 140; -150; -160; 170; 180; -190;"
        " 200; 210; 220; -230; 240; 250; dl_low; ul_high_bb; 260; 270\n"
    ) != NULL);
    assert(strstr(
        contents,
        "; 11; 21; 31; 41; -51; -61; dl_idle; ul_low; 71; 81\n"
    ) != NULL);
    assert(strstr(
        contents,
        "; tc qdisc change root dev eth1 cake bandwidth 28000Kbit\n"
    ) != NULL);
    assert(strstr(
        contents,
        "; 1.0.0.1; 100; 110; 10; 20000; -5; 7; 12; 10000;"
        " -6; 8; 14; 10000\n"
    ) != NULL);
    assert(strstr(contents, "SYSLOG; 20") != NULL);
    assert(strstr(contents, "; Started test process\n") != NULL);
    assert(unlink(path) == 0);
}

static void test_cpu_schema_matches_cake_autorate(void)
{
    char path[] = "/tmp/sqm-mon-log-test-XXXXXX";
    char contents[4096];
    const struct cpu_sample sample = {
        .timestamp_microseconds = 1234567U,
        .count = 2U,
        .counters = {
            { .identifier = "cpu", .user = 1U, .nice = 2U, .system = 3U,
              .idle = 4U, .iowait = 5U, .irq = 6U, .softirq = 7U,
              .steal = 8U, .guest = 9U, .guest_nice = 10U },
            { .identifier = "cpu0", .idle = 50U }
        }
    };
    const unsigned int usage[] = { 40U, 50U };
    int descriptor = mkstemp(path);

    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    log_init("sqm-mon-test", false);
    assert(log_set_file(path, 0U, 0U, 0U, false) == 0);
    log_print_cpu_headers(&sample, true, true);
    log_cpu(&sample, usage);
    log_cpu_raw(&sample);
    log_close();
    read_log(path, contents, sizeof(contents));
    assert(strstr(contents, "CPU_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME; CPU_USAGE; CPU0_USAGE\n") != NULL);
    assert(strstr(contents, "CPU_RAW_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME; CPU_ID; USER; NICE; SYSTEM; IDLE; IOWAIT; IRQ; SIRQ; STEAL; GUEST; GUEST_NICE\n") != NULL);
    assert_record_delimiter_count(strstr(contents, "\nCPU; ") + 1, 5U);
    assert_record_delimiter_count(strstr(contents, "\nCPU_RAW; ") + 1, 14U);
    assert(strstr(contents, "; 1.234567; 40; 50\n") != NULL);
    assert(strstr(contents, "; 1.234567; cpu; 1; 2; 3; 4; 5; 6; 7; 8; 9; 10\n") != NULL);
    assert(unlink(path) == 0);
}

static void test_rotation_export_and_reset_preserve_live_inode(void)
{
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
    assert(log_set_level("info") == 0);
    assert(log_set_file(path, 0U, 1U, 0U, true) == 0);
    log_print_headers(false, true, false, false);
    log_message(LOG_LEVEL_INFO, "%s", large_message);
    assert(stat(path, &after) == 0);
    assert(before.st_ino == after.st_ino);
    (void)snprintf(previous_path, sizeof(previous_path), "%s.old", path);
    read_log(previous_path, contents, sizeof(contents));
    assert(strstr(contents, "before rotation ") != NULL);
    log_message(LOG_LEVEL_INFO, "after rotation");
    assert(log_export_file(export_path, sizeof(export_path)) == 0);
    assert(strcmp(export_path + strlen(export_path) - 3U, ".gz") == 0);
    export_file = gzopen(export_path, "rb");
    assert(export_file != NULL);
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
    mock_time = (struct timespec) { .tv_sec = 1000 };
    log_init("sqm-mon-test", false);
    assert(log_set_file(path, 1U, 0U, 500000U, false) == 0);
    log_print_headers(false, true, false, false);
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

int main(void)
{
    test_operational_syslog();
    test_failed_file_switch_preserves_rotation_path();
    test_debug_logging_to_file();
    test_file_logging_respects_level();
    test_empty_file_path_is_rejected();
    test_log_descriptor_is_close_on_exec();
    test_cake_autorate_headers_and_record_format();
    test_cpu_schema_matches_cake_autorate();
    test_rotation_export_and_reset_preserve_live_inode();
    test_buffer_timeout_and_time_rotation();

    (void)puts("log tests passed");
    return 0;
}
