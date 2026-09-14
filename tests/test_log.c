#define _XOPEN_SOURCE 700

#include "log.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    assert(log_set_file(path) == 0);
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
    assert(log_set_file(path) == 0);
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
    assert(log_set_file("") != 0);
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
    assert(log_set_file(path) == 0);
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

int main(void)
{
    test_debug_logging_to_file();
    test_file_logging_respects_level();
    test_empty_file_path_is_rejected();
    test_cake_autorate_headers_and_record_format();

    (void)puts("log tests passed");
    return 0;
}
