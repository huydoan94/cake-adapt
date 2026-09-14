#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>

#define LOG_MESSAGE_SIZE 2048U
#define TIMED_PAYLOAD_SIZE 2000U
#define LOG_DATETIME_SIZE 20U

static bool log_to_stdout;
static bool log_to_syslog;
static bool debug_to_syslog;
static FILE *log_file;
static enum log_level minimum_log_level = LOG_LEVEL_INFO;

/* cake-autorate 3.3.0-PRERELEASE (ac75f493) analyzer schemas. */
static const char data_header[] =
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
    " CAKE_UL_RATE_KBPS";

static const char load_header[] =
    "LOAD_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
    " DL_ACHIEVED_RATE_KBPS; UL_ACHIEVED_RATE_KBPS;"
    " CAKE_DL_RATE_KBPS; CAKE_UL_RATE_KBPS";

static const char summary_header[] =
    "SUMMARY_HEADER; LOG_DATETIME; LOG_TIMESTAMP; DL_ACHIEVED_RATE_KBPS;"
    " UL_ACHIEVED_RATE_KBPS; DL_SUM_DELAYS; UL_SUM_DELAYS;"
    " DL_AVG_OWD_DELTA_US; UL_AVG_OWD_DELTA_US; DL_LOAD_CONDITION;"
    " UL_LOAD_CONDITION; CAKE_DL_RATE_KBPS; CAKE_UL_RATE_KBPS";

static int syslog_priority(enum log_level level)
{
    switch (level) {
    case LOG_LEVEL_ERROR:
        return LOG_ERR;
    case LOG_LEVEL_WARNING:
        return LOG_WARNING;
    case LOG_LEVEL_NOTICE:
        return LOG_NOTICE;
    case LOG_LEVEL_INFO:
        return LOG_INFO;
    case LOG_LEVEL_DEBUG:
        return LOG_DEBUG;
    }

    return LOG_ERR;
}

static const char *level_name(enum log_level level)
{
    switch (level) {
    case LOG_LEVEL_ERROR:
        return "ERROR";
    case LOG_LEVEL_WARNING:
        return "WARNING";
    case LOG_LEVEL_NOTICE:
        return "INFO";
    case LOG_LEVEL_INFO:
        return "INFO";
    case LOG_LEVEL_DEBUG:
        return "DEBUG";
    }

    return "ERROR";
}

uint64_t log_realtime_microseconds(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_REALTIME, &timestamp) != 0 ||
        timestamp.tv_sec < 0) {
        return 0U;
    }
    return (uint64_t)timestamp.tv_sec * 1000000U +
        (uint64_t)timestamp.tv_nsec / 1000U;
}

static void write_line(const char *line)
{
    if (log_to_stdout) {
        (void)fprintf(stdout, "%s\n", line);
        (void)fflush(stdout);
    }
    if (log_file != NULL) {
        (void)fprintf(log_file, "%s\n", line);
        (void)fflush(log_file);
    }
}

static void write_record_at(
    const char *type,
    const char *message,
    uint64_t timestamp_microseconds
)
{
    char datetime[LOG_DATETIME_SIZE];
    char line[LOG_MESSAGE_SIZE];
    struct tm local_time;
    time_t seconds = (time_t)(timestamp_microseconds / 1000000U);

    if (localtime_r(&seconds, &local_time) == NULL ||
        strftime(
            datetime,
            sizeof(datetime),
            "%Y-%m-%d-%H:%M:%S",
            &local_time
        ) == 0U) {
        (void)snprintf(datetime, sizeof(datetime), "1970-01-01-00:00:00");
    }

    (void)snprintf(
        line,
        sizeof(line),
        "%s; %s; %" PRIu64 ".%06" PRIu64 "; %s",
        type,
        datetime,
        timestamp_microseconds / 1000000U,
        timestamp_microseconds % 1000000U,
        message
    );
    write_line(line);
}

static void write_record(
    const char *type,
    const char *message
)
{
    write_record_at(type, message, log_realtime_microseconds());
}

void log_init(
    const char *identifier,
    bool foreground
)
{
    log_to_stdout = foreground;
    log_to_syslog = !foreground;
    debug_to_syslog = false;

    if (log_to_syslog) {
        openlog(identifier, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
}

void log_close(void)
{
    if (log_file != NULL) {
        (void)fclose(log_file);
        log_file = NULL;
    }

    if (log_to_syslog) {
        closelog();
        log_to_syslog = false;
    }
}

int log_set_file(const char *path)
{
    FILE *file;
    int descriptor_flags;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    file = fopen(path, "a");
    if (file == NULL) {
        return -1;
    }
    descriptor_flags = fcntl(fileno(file), F_GETFD);
    if (descriptor_flags < 0 ||
        fcntl(fileno(file), F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        int saved_errno = errno;

        (void)fclose(file);
        errno = saved_errno;
        return -1;
    }

    if (log_file != NULL) {
        (void)fclose(log_file);
    }
    log_file = file;

    return 0;
}

int log_set_level(const char *level)
{
    if (strcasecmp(level, "error") == 0) {
        minimum_log_level = LOG_LEVEL_ERROR;
    } else if (strcasecmp(level, "warning") == 0) {
        minimum_log_level = LOG_LEVEL_WARNING;
    } else if (strcasecmp(level, "notice") == 0) {
        minimum_log_level = LOG_LEVEL_NOTICE;
    } else if (strcasecmp(level, "info") == 0) {
        minimum_log_level = LOG_LEVEL_INFO;
    } else if (strcasecmp(level, "debug") == 0) {
        minimum_log_level = LOG_LEVEL_DEBUG;
    } else {
        return -1;
    }

    return 0;
}

void log_set_debug_syslog(bool enabled)
{
    debug_to_syslog = enabled;
}

void log_print_headers(
    bool output_processing_stats,
    bool output_load_stats,
    bool output_summary_stats
)
{
    if (output_processing_stats) {
        write_line(data_header);
    }
    if (output_load_stats) {
        write_line(load_header);
    }
    if (output_summary_stats) {
        write_line(summary_header);
    }
}

static void write_formatted_record(
    const char *type,
    const char *format,
    ...
)
{
    char message[LOG_MESSAGE_SIZE];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    write_record(type, message);
}

static void write_timed_record(
    const char *type,
    const char *format,
    ...
)
{
    char payload[TIMED_PAYLOAD_SIZE];
    char message[LOG_MESSAGE_SIZE];
    uint64_t processing_time_microseconds = log_realtime_microseconds();
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(payload, sizeof(payload), format, arguments);
    va_end(arguments);

    (void)snprintf(
        message,
        sizeof(message),
        "%" PRIu64 ".%06" PRIu64 "; %s",
        processing_time_microseconds / 1000000U,
        processing_time_microseconds % 1000000U,
        payload
    );
    write_record(type, message);
}

void log_load(const struct log_load_record *record)
{
    write_timed_record(
        "LOAD",
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
        "DATA",
        "%" PRIu64 "; %" PRIu64 "; %u; %u;"
        " %" PRIu64 ".%06" PRIu64 "; %s; %" PRIu64 ";"
        " %" PRIu32 "; %" PRIu32 "; %" PRId64 "; %" PRId64
        "; %" PRIu64 "; %" PRIu32 "; %" PRIu32 "; %" PRId64 ";"
        " %" PRId64 "; %" PRIu64 "; %u; %" PRId64 "; %" PRIu64
        "; %" PRIu64 "; %u; %" PRId64 "; %" PRIu64 "; %" PRIu64
        "; %s; %s; %" PRIu64 "; %" PRIu64,
        record->download_achieved_rate_kbps,
        record->upload_achieved_rate_kbps,
        record->download_load_percent,
        record->upload_load_percent,
        record->icmp_timestamp_microseconds / 1000000U,
        record->icmp_timestamp_microseconds % 1000000U,
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
        "SUMMARY",
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

void log_shaper(
    const char *interface,
    uint64_t rate_kbps
)
{
    write_formatted_record(
        "SHAPER",
        "tc qdisc change root dev %s cake bandwidth %" PRIu64 "Kbit",
        interface,
        rate_kbps
    );
}

void log_system_message(
    const char *format,
    ...
)
{
    char message[LOG_MESSAGE_SIZE];
    va_list arguments;
    uint64_t timestamp_microseconds = log_realtime_microseconds();

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    if (log_to_syslog) {
        syslog(
            LOG_INFO,
            "INFO: %" PRIu64 ".%06" PRIu64 " %s",
            timestamp_microseconds / 1000000U,
            timestamp_microseconds % 1000000U,
            message
        );
    }
    write_record_at("SYSLOG", message, timestamp_microseconds);
}

void log_message(
    enum log_level level,
    const char *format,
    ...
)
{
    char message[LOG_MESSAGE_SIZE];
    va_list arguments;
    uint64_t timestamp_microseconds;

    if (level > minimum_log_level) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    timestamp_microseconds = log_realtime_microseconds();
    if (log_to_syslog &&
        (level == LOG_LEVEL_ERROR ||
            (level == LOG_LEVEL_DEBUG && debug_to_syslog))) {
        syslog(
            syslog_priority(level),
            "%s: %" PRIu64 ".%06" PRIu64 " %s",
            level_name(level),
            timestamp_microseconds / 1000000U,
            timestamp_microseconds % 1000000U,
            message
        );
    }
    write_record_at(level_name(level), message, timestamp_microseconds);
}
