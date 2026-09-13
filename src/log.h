#ifndef SQM_MON_LOG_H
#define SQM_MON_LOG_H

#include <stdbool.h>
#include <stdint.h>

enum log_level {
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_NOTICE,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG
};

void log_init(
    const char *identifier,
    bool foreground
);

void log_close(void);

int log_set_file(const char *path);

int log_set_level(const char *level);

void log_set_debug_syslog(bool enabled);

void log_print_headers(
    bool output_processing_stats,
    bool output_load_stats,
    bool output_summary_stats
);

uint64_t log_realtime_microseconds(void);

void log_record(
    const char *type,
    const char *format,
    ...
) __attribute__((format(printf, 2, 3)));

void log_system_message(
    const char *format,
    ...
) __attribute__((format(printf, 1, 2)));

void log_message(
    enum log_level level,
    const char *format,
    ...
) __attribute__((format(printf, 2, 3)));

#endif
