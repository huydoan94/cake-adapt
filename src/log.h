#ifndef SQM_MON_LOG_H
#define SQM_MON_LOG_H

#include <stdbool.h>

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

int log_set_level(const char *level);

void log_message(
    enum log_level level,
    const char *format,
    ...
) __attribute__((format(printf, 2, 3)));

#endif
