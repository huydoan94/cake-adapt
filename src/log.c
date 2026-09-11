#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <syslog.h>

#define SQM_MON_LOG_MESSAGE_SIZE 512U

static bool log_to_stderr;
static enum log_level minimum_log_level = LOG_LEVEL_INFO;

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
        return "error";
    case LOG_LEVEL_WARNING:
        return "warning";
    case LOG_LEVEL_NOTICE:
        return "notice";
    case LOG_LEVEL_INFO:
        return "info";
    case LOG_LEVEL_DEBUG:
        return "debug";
    }

    return "unknown";
}

void log_init(
    const char *identifier,
    bool foreground
)
{
    log_to_stderr = foreground;
    openlog(identifier, LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

void log_close(void)
{
    closelog();
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

void log_message(
    enum log_level level,
    const char *format,
    ...
)
{
    char message[SQM_MON_LOG_MESSAGE_SIZE];
    va_list arguments;

    if (level > minimum_log_level) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    syslog(syslog_priority(level), "%s", message);

    if (log_to_stderr) {
        (void)fprintf(stderr, "%s: %s\n", level_name(level), message);
    }
}
