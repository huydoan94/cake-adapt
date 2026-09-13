#define _GNU_SOURCE

#include "latency.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FPING_PATH "/usr/bin/fping"
#define FPING_PERIOD_MILLISECONDS "1000"
#define FPING_RESPONSE_INTERVAL_MILLISECONDS "10"
#define FPING_TIMEOUT_MILLISECONDS "10000"
#define CHILD_STOP_ATTEMPTS 50U
#define CHILD_STOP_INTERVAL_NANOSECONDS 10000000L
#define BASELINE_SCALE 1000U
#define BASELINE_INCREASE_WEIGHT 1U
#define BASELINE_DECREASE_WEIGHT 900U
#define DELTA_EWMA_WEIGHT 95U

static void set_error(
    char *error,
    size_t error_size,
    const char *format,
    ...
)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static bool elapsed_microseconds(
    const struct timespec *start,
    const struct timespec *end,
    uint64_t *elapsed
)
{
    time_t seconds;
    long nanoseconds;

    seconds = end->tv_sec - start->tv_sec;
    nanoseconds = end->tv_nsec - start->tv_nsec;

    if (nanoseconds < 0L) {
        --seconds;
        nanoseconds += 1000000000L;
    }

    if (seconds < 0 ||
        (uint64_t)seconds > UINT64_MAX / 1000000U) {
        return false;
    }

    *elapsed = (uint64_t)seconds * 1000000U +
        (uint64_t)nanoseconds / 1000U;
    return true;
}

static int remaining_timeout(
    const struct timespec *start,
    int timeout_milliseconds
)
{
    struct timespec current;
    uint64_t elapsed;
    uint64_t timeout_microseconds;
    uint64_t remaining_microseconds;

    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0 ||
        !elapsed_microseconds(start, &current, &elapsed)) {
        return 0;
    }

    timeout_microseconds = (uint64_t)timeout_milliseconds * 1000U;
    if (elapsed >= timeout_microseconds) {
        return 0;
    }

    remaining_microseconds = timeout_microseconds - elapsed;
    /* poll() accepts milliseconds, so round up to preserve the deadline. */
    return (int)((remaining_microseconds + 999U) / 1000U);
}

static bool parse_unsigned(
    const char *start,
    const char *end,
    uint64_t *value
)
{
    const char *character;
    uint64_t result = 0U;

    if (start == end) {
        return false;
    }

    for (character = start; character < end; character++) {
        unsigned int digit;

        if (*character < '0' || *character > '9') {
            return false;
        }
        digit = (unsigned int)(*character - '0');
        if (result > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }

    *value = result;
    return true;
}

static bool parse_timestamp(
    const char *line,
    const char **remainder,
    uint64_t *timestamp_microseconds
)
{
    const char *closing_bracket;
    const char *decimal_point;
    const char *fraction_end;
    const char *character;
    uint64_t fraction = 0U;
    uint64_t seconds;
    size_t retained_digits = 0U;

    if (line[0] != '[') {
        return false;
    }
    closing_bracket = strchr(line + 1, ']');
    if (closing_bracket == NULL || closing_bracket[1] != ' ') {
        return false;
    }
    decimal_point = memchr(
        line + 1,
        '.',
        (size_t)(closing_bracket - (line + 1))
    );
    if (decimal_point == NULL ||
        !parse_unsigned(line + 1, decimal_point, &seconds)) {
        return false;
    }

    fraction_end = closing_bracket;
    if (decimal_point + 1 == fraction_end) {
        return false;
    }
    for (character = decimal_point + 1;
         character < fraction_end;
         character++) {
        if (*character < '0' || *character > '9') {
            return false;
        }
        if (retained_digits < 6U) {
            fraction = fraction * 10U +
                (uint64_t)(unsigned int)(*character - '0');
            ++retained_digits;
        }
    }
    while (retained_digits < 6U) {
        fraction *= 10U;
        ++retained_digits;
    }

    if (seconds > (UINT64_MAX - fraction) / 1000000U) {
        return false;
    }
    *timestamp_microseconds = seconds * 1000000U + fraction;
    *remainder = closing_bracket + 2;
    return true;
}

enum latency_fping_line_result latency_parse_fping_line(
    const char *target,
    const char *line,
    struct latency_sample *sample
)
{
    const char *cursor;
    const char *sequence_end;
    const char *target_end;
    char *rtt_end;
    uint64_t sequence;
    uint64_t timestamp_microseconds;
    double round_trip_milliseconds;
    double round_trip_microseconds;
    size_t target_length;

    if (target == NULL || line == NULL || sample == NULL ||
        !parse_timestamp(line, &cursor, &timestamp_microseconds)) {
        return LATENCY_FPING_LINE_INVALID;
    }

    target_end = strstr(cursor, " : [");
    if (target_end == NULL) {
        return LATENCY_FPING_LINE_INVALID;
    }
    target_length = (size_t)(target_end - cursor);
    if (strlen(target) != target_length ||
        memcmp(cursor, target, target_length) != 0) {
        return LATENCY_FPING_LINE_INVALID;
    }

    cursor = target_end + strlen(" : [");
    sequence_end = strchr(cursor, ']');
    if (sequence_end == NULL ||
        !parse_unsigned(cursor, sequence_end, &sequence)) {
        return LATENCY_FPING_LINE_INVALID;
    }
    cursor = sequence_end + 1;
    if (strncmp(cursor, ", timed out", strlen(", timed out")) == 0) {
        return LATENCY_FPING_LINE_TIMEOUT;
    }
    if (strncmp(cursor, ", ", 2U) != 0) {
        return LATENCY_FPING_LINE_INVALID;
    }

    cursor += 2;
    target_end = cursor;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
    }
    if (cursor == target_end ||
        strncmp(cursor, " bytes, ", strlen(" bytes, ")) != 0) {
        return LATENCY_FPING_LINE_INVALID;
    }
    cursor += strlen(" bytes, ");
    errno = 0;
    round_trip_milliseconds = strtod(cursor, &rtt_end);
    if (errno == ERANGE || rtt_end == cursor ||
        !isfinite(round_trip_milliseconds) ||
        round_trip_milliseconds < 0.0 ||
        strncmp(rtt_end, " ms", strlen(" ms")) != 0) {
        return LATENCY_FPING_LINE_INVALID;
    }

    round_trip_microseconds = round_trip_milliseconds * 1000.0;
    if (round_trip_microseconds > (double)UINT32_MAX) {
        sample->round_trip_microseconds = UINT32_MAX;
    } else {
        sample->round_trip_microseconds =
            (uint32_t)(round_trip_microseconds + 0.5);
    }
    sample->timestamp_microseconds = timestamp_microseconds;
    sample->sequence = sequence;
    return LATENCY_FPING_LINE_SAMPLE;
}

static int set_nonblocking(
    int descriptor,
    char *error,
    size_t error_size
)
{
    int flags = fcntl(descriptor, F_GETFL);

    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        set_error(
            error,
            error_size,
            "could not make fping pipe nonblocking: %s",
            strerror(errno)
        );
        return -1;
    }
    return 0;
}

static void close_pipe(int descriptors[2])
{
    if (descriptors[0] >= 0) {
        (void)close(descriptors[0]);
        descriptors[0] = -1;
    }
    if (descriptors[1] >= 0) {
        (void)close(descriptors[1]);
        descriptors[1] = -1;
    }
}

static void stop_child(pid_t process_identifier)
{
    const struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = CHILD_STOP_INTERVAL_NANOSECONDS
    };
    unsigned int attempt;

    if (process_identifier <= 0) {
        return;
    }

    (void)kill(process_identifier, SIGTERM);
    for (attempt = 0U; attempt < CHILD_STOP_ATTEMPTS; attempt++) {
        pid_t result = waitpid(process_identifier, NULL, WNOHANG);

        if (result == process_identifier ||
            (result < 0 && errno == ECHILD)) {
            return;
        }
        if (result < 0 && errno != EINTR) {
            return;
        }
        (void)nanosleep(&interval, NULL);
    }

    (void)kill(process_identifier, SIGKILL);
    while (waitpid(process_identifier, NULL, 0) < 0 && errno == EINTR) {
    }
}

static int read_exec_result(
    int descriptor,
    int *exec_error
)
{
    unsigned char *destination = (unsigned char *)(void *)exec_error;
    size_t received = 0U;

    while (received < sizeof(*exec_error)) {
        ssize_t result = read(
            descriptor,
            destination + received,
            sizeof(*exec_error) - received
        );

        if (result == 0) {
            return received == 0U ? 0 : -1;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        received += (size_t)result;
    }
    return 1;
}

static int start_fping(
    struct sqm_mon_latency *latency,
    const char *interface,
    const char *target,
    char *error,
    size_t error_size
)
{
    int output_pipe[2] = { -1, -1 };
    int diagnostic_pipe[2] = { -1, -1 };
    int exec_pipe[2] = { -1, -1 };
    sigset_t child_signal_mask;
    pid_t process_identifier;
    int exec_error = 0;
    int exec_result;

    if (sigemptyset(&child_signal_mask) != 0) {
        set_error(
            error,
            error_size,
            "could not prepare fping signal mask: %s",
            strerror(errno)
        );
        return -1;
    }
    if (pipe2(output_pipe, O_CLOEXEC) != 0 ||
        pipe2(diagnostic_pipe, O_CLOEXEC) != 0 ||
        pipe2(exec_pipe, O_CLOEXEC) != 0) {
        set_error(
            error,
            error_size,
            "could not create fping pipe: %s",
            strerror(errno)
        );
        close_pipe(output_pipe);
        close_pipe(diagnostic_pipe);
        close_pipe(exec_pipe);
        return -1;
    }

    process_identifier = fork();
    if (process_identifier < 0) {
        set_error(
            error,
            error_size,
            "could not start fping: %s",
            strerror(errno)
        );
        close_pipe(output_pipe);
        close_pipe(diagnostic_pipe);
        close_pipe(exec_pipe);
        return -1;
    }

    if (process_identifier == 0) {
        char *const arguments[] = {
            (char *)FPING_PATH,
            (char *)"-4",
            (char *)"-I",
            (char *)interface,
            (char *)"--timestamp",
            (char *)"--loop",
            (char *)"--period",
            (char *)FPING_PERIOD_MILLISECONDS,
            (char *)"--interval",
            (char *)FPING_RESPONSE_INTERVAL_MILLISECONDS,
            (char *)"--timeout",
            (char *)FPING_TIMEOUT_MILLISECONDS,
            (char *)target,
            NULL
        };
        int child_error = 0;

        (void)close(output_pipe[0]);
        (void)close(diagnostic_pipe[0]);
        (void)close(exec_pipe[0]);
        if (sigprocmask(SIG_SETMASK, &child_signal_mask, NULL) != 0 ||
            dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(diagnostic_pipe[1], STDERR_FILENO) < 0) {
            child_error = errno;
        } else {
            (void)close(output_pipe[1]);
            (void)close(diagnostic_pipe[1]);
            execv(FPING_PATH, arguments);
            child_error = errno;
        }
        while (write(exec_pipe[1], &child_error, sizeof(child_error)) < 0 &&
               errno == EINTR) {
        }
        _exit(127);
    }

    (void)close(output_pipe[1]);
    output_pipe[1] = -1;
    (void)close(diagnostic_pipe[1]);
    diagnostic_pipe[1] = -1;
    (void)close(exec_pipe[1]);
    exec_pipe[1] = -1;

    exec_result = read_exec_result(exec_pipe[0], &exec_error);
    close_pipe(exec_pipe);
    if (exec_result != 0) {
        set_error(
            error,
            error_size,
            exec_result > 0
                ? "could not execute fping: %s"
                : "could not confirm fping startup: %s",
            exec_result > 0 ? strerror(exec_error) : strerror(errno)
        );
        close_pipe(output_pipe);
        close_pipe(diagnostic_pipe);
        stop_child(process_identifier);
        return -1;
    }

    if (set_nonblocking(output_pipe[0], error, error_size) != 0 ||
        set_nonblocking(diagnostic_pipe[0], error, error_size) != 0) {
        close_pipe(output_pipe);
        close_pipe(diagnostic_pipe);
        stop_child(process_identifier);
        return -1;
    }

    latency->output_descriptor = output_pipe[0];
    latency->diagnostic_descriptor = diagnostic_pipe[0];
    latency->process_identifier = process_identifier;
    latency->output_length = 0U;
    return 0;
}

static int take_output_line(
    struct sqm_mon_latency *latency,
    char *line,
    size_t line_size
)
{
    char *newline = memchr(
        latency->output_buffer,
        '\n',
        latency->output_length
    );
    size_t length;
    size_t consumed;

    if (newline == NULL) {
        return 0;
    }

    length = (size_t)(newline - latency->output_buffer);
    consumed = length + 1U;
    if (length > 0U && latency->output_buffer[length - 1U] == '\r') {
        --length;
    }
    if (length >= line_size) {
        return -1;
    }

    memcpy(line, latency->output_buffer, length);
    line[length] = '\0';
    memmove(
        latency->output_buffer,
        latency->output_buffer + consumed,
        latency->output_length - consumed
    );
    latency->output_length -= consumed;
    return 1;
}

static void set_child_exit_error(
    struct sqm_mon_latency *latency,
    char *error,
    size_t error_size
)
{
    int status;
    pid_t result;

    result = waitpid(latency->process_identifier, &status, WNOHANG);
    if (result == latency->process_identifier) {
        latency->process_identifier = -1;
        if (WIFEXITED(status)) {
            set_error(
                error,
                error_size,
                "fping exited with status %d",
                WEXITSTATUS(status)
            );
        } else if (WIFSIGNALED(status)) {
            set_error(
                error,
                error_size,
                "fping terminated by signal %d",
                WTERMSIG(status)
            );
        } else {
            set_error(error, error_size, "fping stopped unexpectedly");
        }
        return;
    }

    set_error(error, error_size, "fping output closed unexpectedly");
}

void latency_init(struct sqm_mon_latency *latency)
{
    *latency = (struct sqm_mon_latency) {
        .output_descriptor = -1,
        .diagnostic_descriptor = -1,
        .process_identifier = -1,
        .target = "",
        .output_buffer = "",
        .output_length = 0U
    };
}

bool latency_is_open(const struct sqm_mon_latency *latency)
{
    return latency->output_descriptor >= 0 &&
        latency->diagnostic_descriptor >= 0 &&
        latency->process_identifier > 0;
}

int latency_open(
    struct sqm_mon_latency *latency,
    const char *interface,
    const char *target,
    char *error,
    size_t error_size
)
{
    struct in_addr target_address;

    if (inet_pton(AF_INET, target, &target_address) != 1) {
        set_error(
            error,
            error_size,
            "latency target '%s' is not an IPv4 address",
            target
        );
        return -1;
    }
    if (snprintf(latency->target, sizeof(latency->target), "%s", target) < 0) {
        set_error(error, error_size, "could not store latency target");
        return -1;
    }
    if (start_fping(
            latency,
            interface,
            target,
            error,
            error_size
        ) != 0) {
        latency->target[0] = '\0';
        return -1;
    }
    return 0;
}

void latency_close(struct sqm_mon_latency *latency)
{
    pid_t process_identifier = latency->process_identifier;

    if (latency->output_descriptor >= 0) {
        (void)close(latency->output_descriptor);
    }
    if (latency->diagnostic_descriptor >= 0) {
        (void)close(latency->diagnostic_descriptor);
    }
    latency->output_descriptor = -1;
    latency->diagnostic_descriptor = -1;
    latency->process_identifier = -1;
    latency->target[0] = '\0';
    latency->output_length = 0U;
    stop_child(process_identifier);
}

void latency_tracker_init(struct latency_tracker *tracker)
{
    tracker->baseline_scaled = 0U;
    tracker->delta_ewma_microseconds = 0;
    tracker->initialized = false;
}

void latency_tracker_update(
    struct latency_tracker *tracker,
    const struct latency_sample *sample,
    struct latency_observation *observation
)
{
    uint64_t sample_scaled =
        (uint64_t)sample->round_trip_microseconds * BASELINE_SCALE;
    unsigned int sample_weight;

    if (!tracker->initialized) {
        tracker->baseline_scaled = sample_scaled;
        tracker->initialized = true;
    } else {
        /*
         * Keep three decimal places for the integer EWMA. Lower RTT samples
         * get 90% weight while higher samples get 0.1%, so the baseline falls
         * quickly but does not absorb transient queueing delay.
         */
        sample_weight = sample_scaled < tracker->baseline_scaled
            ? BASELINE_DECREASE_WEIGHT
            : BASELINE_INCREASE_WEIGHT;
        tracker->baseline_scaled = (
            tracker->baseline_scaled * (BASELINE_SCALE - sample_weight) +
            sample_scaled * sample_weight + BASELINE_SCALE / 2U
        ) / BASELINE_SCALE;
    }

    observation->round_trip_microseconds =
        sample->round_trip_microseconds;
    observation->baseline_microseconds = (uint32_t)(
        (tracker->baseline_scaled + BASELINE_SCALE / 2U) /
            BASELINE_SCALE
    );
    observation->delta_microseconds =
        (int64_t)sample->round_trip_microseconds -
        (int64_t)observation->baseline_microseconds;
    observation->delta_ewma_microseconds = tracker->delta_ewma_microseconds;
    observation->timestamp_microseconds = sample->timestamp_microseconds;
    observation->sequence = sample->sequence;
}

void latency_tracker_update_delta_ewma(
    struct latency_tracker *tracker,
    bool low_load,
    struct latency_observation *observation
)
{
    /* cake-autorate freezes reflector delay EWMA while either link is busy. */
    if (low_load) {
        /* An RTT probe represents equal download and upload one-way delay. */
        tracker->delta_ewma_microseconds =
            ((int64_t)DELTA_EWMA_WEIGHT *
                    (observation->delta_microseconds / 2) +
                (int64_t)(1000U - DELTA_EWMA_WEIGHT) *
                    tracker->delta_ewma_microseconds) /
            1000;
    }
    observation->delta_ewma_microseconds =
        tracker->delta_ewma_microseconds;
}

enum latency_probe_result latency_probe(
    struct sqm_mon_latency *latency,
    int timeout_milliseconds,
    struct latency_sample *sample,
    char *error,
    size_t error_size
)
{
    struct pollfd descriptors[2] = {
        {
            .fd = latency->output_descriptor,
            .events = POLLIN,
            .revents = 0
        },
        {
            .fd = latency->diagnostic_descriptor,
            .events = POLLIN,
            .revents = 0
        }
    };
    struct timespec started_at;
    bool output_closed = false;
    bool result_available = false;
    enum latency_probe_result latest_result = LATENCY_PROBE_TIMEOUT;
    struct latency_sample latest_sample = { 0U, 0U, 0U };

    if (timeout_milliseconds <= 0) {
        set_error(error, error_size, "latency timeout must be positive");
        return LATENCY_PROBE_ERROR;
    }
    if (!latency_is_open(latency)) {
        set_error(error, error_size, "fping is not running");
        return LATENCY_PROBE_ERROR;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &started_at) != 0) {
        set_error(
            error,
            error_size,
            "could not read monotonic clock: %s",
            strerror(errno)
        );
        return LATENCY_PROBE_ERROR;
    }

    for (;;) {
        for (;;) {
            char line[SQM_MON_LATENCY_OUTPUT_SIZE];
            struct latency_sample parsed_sample;
            int line_result = take_output_line(
                latency,
                line,
                sizeof(line)
            );

            if (line_result < 0) {
                set_error(error, error_size, "fping output line is too long");
                return LATENCY_PROBE_ERROR;
            }
            if (line_result == 0) {
                break;
            }

            {
                enum latency_fping_line_result parse_result =
                    latency_parse_fping_line(
                        latency->target,
                        line,
                        &parsed_sample
                    );

                if (parse_result == LATENCY_FPING_LINE_INVALID) {
                    set_error(
                        error,
                        error_size,
                        "unexpected fping output: %.160s",
                        line
                    );
                    return LATENCY_PROBE_ERROR;
                }
                result_available = true;
                if (parse_result == LATENCY_FPING_LINE_SAMPLE) {
                    latest_result = LATENCY_PROBE_SUCCESS;
                    latest_sample = parsed_sample;
                } else {
                    latest_result = LATENCY_PROBE_TIMEOUT;
                }
            }
        }

        if (latency->output_length == sizeof(latency->output_buffer)) {
            set_error(error, error_size, "fping output line is too long");
            return LATENCY_PROBE_ERROR;
        }

        {
            char diagnostic[256];
            ssize_t received = read(
                latency->diagnostic_descriptor,
                diagnostic,
                sizeof(diagnostic) - 1U
            );

            if (received > 0) {
                size_t length = (size_t)received;

                diagnostic[length] = '\0';
                while (length > 0U &&
                       (diagnostic[length - 1U] == '\n' ||
                        diagnostic[length - 1U] == '\r')) {
                    diagnostic[--length] = '\0';
                }
                set_error(
                    error,
                    error_size,
                    "fping reported: %.180s",
                    diagnostic
                );
                return LATENCY_PROBE_ERROR;
            }
            if (received < 0 && errno != EAGAIN && errno != EINTR) {
                set_error(
                    error,
                    error_size,
                    "could not read fping diagnostics: %s",
                    strerror(errno)
                );
                return LATENCY_PROBE_ERROR;
            }
        }

        if (!output_closed) {
            ssize_t received = read(
                latency->output_descriptor,
                latency->output_buffer + latency->output_length,
                sizeof(latency->output_buffer) - latency->output_length
            );

            if (received > 0) {
                latency->output_length += (size_t)received;
                continue;
            }
            if (received == 0) {
                output_closed = true;
            } else if (errno != EAGAIN && errno != EINTR) {
                set_error(
                    error,
                    error_size,
                    "could not read fping output: %s",
                    strerror(errno)
                );
                return LATENCY_PROBE_ERROR;
            }
        }

        if (result_available) {
            if (latest_result == LATENCY_PROBE_SUCCESS) {
                *sample = latest_sample;
            }
            return latest_result;
        }
        if (output_closed) {
            set_child_exit_error(latency, error, error_size);
            return LATENCY_PROBE_ERROR;
        }

        descriptors[0].revents = 0;
        descriptors[1].revents = 0;
        {
            int timeout = remaining_timeout(&started_at, timeout_milliseconds);
            int poll_result;

            if (timeout == 0) {
                return LATENCY_PROBE_TIMEOUT;
            }
            poll_result = poll(descriptors, 2U, timeout);
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                set_error(
                    error,
                    error_size,
                    "could not wait for fping output: %s",
                    strerror(errno)
                );
                return LATENCY_PROBE_ERROR;
            }
            if (poll_result == 0) {
                return LATENCY_PROBE_TIMEOUT;
            }
        }

        if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0 ||
            (descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
            set_error(error, error_size, "fping pipe reported an error");
            return LATENCY_PROBE_ERROR;
        }
        if ((descriptors[1].revents & POLLHUP) != 0 &&
            (descriptors[0].revents & POLLHUP) == 0) {
            set_error(
                error,
                error_size,
                "fping diagnostic stream closed unexpectedly"
            );
            return LATENCY_PROBE_ERROR;
        }
    }
}
