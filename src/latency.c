#define _GNU_SOURCE

#include "latency.h"
#include "error.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <spawn.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FPING_PATH "/usr/bin/fping"
#define FPING_TIMEOUT_MILLISECONDS "10000"
#define CHILD_STOP_ATTEMPTS 50U
#define CHILD_STOP_INTERVAL_NANOSECONDS 10000000L
#define BASELINE_SCALE 1000U
#define BASELINE_INCREASE_WEIGHT 1U
#define BASELINE_DECREASE_WEIGHT 900U
#define DELTA_EWMA_WEIGHT 95U

extern char **environ;

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
    const char *line,
    struct latency_sample *sample
)
{
    const char *cursor;
    const char *separator;
    const char *sequence_end;
    const char *target_end;
    char *rtt_end;
    uint64_t sequence;
    uint64_t timestamp_microseconds;
    double round_trip_milliseconds;
    double round_trip_microseconds;
    size_t target_length;

    if (line == NULL || sample == NULL ||
        !parse_timestamp(line, &cursor, &timestamp_microseconds)) {
        return LATENCY_FPING_LINE_INVALID;
    }

    separator = strstr(cursor, " : [");
    if (separator == NULL) {
        return LATENCY_FPING_LINE_INVALID;
    }
    target_end = separator;
    while (target_end > cursor &&
           (target_end[-1] == ' ' || target_end[-1] == '\t')) {
        target_end--;
    }
    target_length = (size_t)(target_end - cursor);
    if (target_length == 0U || target_length >= sizeof(sample->target)) {
        return LATENCY_FPING_LINE_INVALID;
    }
    memcpy(sample->target, cursor, target_length);
    sample->target[target_length] = '\0';

    cursor = separator + strlen(" : [");
    sequence_end = strchr(cursor, ']');
    if (sequence_end == NULL ||
        !parse_unsigned(cursor, sequence_end, &sequence)) {
        return LATENCY_FPING_LINE_INVALID;
    }
    sample->round_trip_microseconds = 0U;
    sample->timestamp_microseconds = timestamp_microseconds;
    sample->sequence = sequence;
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
        error_set(
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

static int spawn_fping(
    pid_t *process_identifier,
    const int output_pipe[2],
    const int diagnostic_pipe[2],
    char *const arguments[],
    char *error,
    size_t error_size
)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    sigset_t child_signal_mask;
    int result;

    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        goto failed;
    }
    if ((result = posix_spawn_file_actions_addclose(
            &actions,
            output_pipe[0]
        )) != 0 ||
        (result = posix_spawn_file_actions_addclose(
            &actions,
            diagnostic_pipe[0]
        )) != 0 ||
        (result = posix_spawn_file_actions_adddup2(
            &actions,
            output_pipe[1],
            STDOUT_FILENO
        )) != 0 ||
        (result = posix_spawn_file_actions_adddup2(
            &actions,
            diagnostic_pipe[1],
            STDERR_FILENO
        )) != 0 ||
        (result = posix_spawn_file_actions_addclose(
            &actions,
            output_pipe[1]
        )) != 0 ||
        (result = posix_spawn_file_actions_addclose(
            &actions,
            diagnostic_pipe[1]
        )) != 0) {
        goto destroy_actions;
    }

    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        goto destroy_actions;
    }
    if (sigemptyset(&child_signal_mask) != 0) {
        result = errno;
        goto destroy_attributes;
    }

    result = posix_spawnattr_setsigmask(&attributes, &child_signal_mask);
    if (result == 0) {
        result = posix_spawnattr_setflags(
            &attributes,
            POSIX_SPAWN_SETSIGMASK
        );
    }
    if (result == 0) {
        result = posix_spawn(
            process_identifier,
            FPING_PATH,
            &actions,
            &attributes,
            arguments,
            environ
        );
    }

destroy_attributes:
    (void)posix_spawnattr_destroy(&attributes);
destroy_actions:
    (void)posix_spawn_file_actions_destroy(&actions);
failed:
    if (result != 0) {
        error_set(
            error,
            error_size,
            "could not start fping: %s",
            strerror(result)
        );
        return -1;
    }
    return 0;
}

static int start_fping(
    struct sqm_mon_latency *latency,
    const char *interface,
    const char *const *targets,
    size_t target_count,
    uint64_t reflector_ping_interval_microseconds,
    char *error,
    size_t error_size
)
{
    char period_milliseconds[32];
    char response_interval_milliseconds[32];
    char **arguments;
    int output_pipe[2] = { -1, -1 };
    int diagnostic_pipe[2] = { -1, -1 };
    pid_t process_identifier;
    uint64_t period;
    uint64_t response_interval;
    size_t index;

    if (target_count == 0U) {
        error_set(error, error_size, "fping requires at least one target");
        return -1;
    }
    period = reflector_ping_interval_microseconds / 1000U;
    if (reflector_ping_interval_microseconds % 1000U >= 500U) {
        period++;
    }
    response_interval =
        reflector_ping_interval_microseconds / target_count / 1000U;
    if (period == 0U || response_interval == 0U ||
        snprintf(
            period_milliseconds,
            sizeof(period_milliseconds),
            "%" PRIu64,
            period
        ) < 0 ||
        snprintf(
            response_interval_milliseconds,
            sizeof(response_interval_milliseconds),
            "%" PRIu64,
            response_interval
        ) < 0) {
        error_set(error, error_size, "fping interval is too short");
        return -1;
    }

    if (target_count > SIZE_MAX - 13U) {
        error_set(error, error_size, "too many fping targets");
        return -1;
    }
    arguments = calloc(target_count + 13U, sizeof(*arguments));
    if (arguments == NULL) {
        error_set(
            error,
            error_size,
            "could not allocate fping arguments: %s",
            strerror(errno)
        );
        return -1;
    }
    arguments[0] = (char *)FPING_PATH;
    arguments[1] = (char *)"-4";
    arguments[2] = (char *)"-I";
    arguments[3] = (char *)interface;
    arguments[4] = (char *)"--timestamp";
    arguments[5] = (char *)"--loop";
    arguments[6] = (char *)"--period";
    arguments[7] = period_milliseconds;
    arguments[8] = (char *)"--interval";
    arguments[9] = response_interval_milliseconds;
    arguments[10] = (char *)"--timeout";
    arguments[11] = (char *)FPING_TIMEOUT_MILLISECONDS;
    for (index = 0U; index < target_count; index++) {
        arguments[12U + index] = (char *)targets[index];
    }

    if (pipe2(output_pipe, O_CLOEXEC) != 0 ||
        pipe2(diagnostic_pipe, O_CLOEXEC) != 0) {
        error_set(
            error,
            error_size,
            "could not create fping pipe: %s",
            strerror(errno)
        );
        close_pipe(output_pipe);
        close_pipe(diagnostic_pipe);
        free(arguments);
        return -1;
    }

    if (spawn_fping(
            &process_identifier,
            output_pipe,
            diagnostic_pipe,
            arguments,
            error,
            error_size
        ) != 0) {
        close_pipe(output_pipe);
        close_pipe(diagnostic_pipe);
        free(arguments);
        return -1;
    }

    (void)close(output_pipe[1]);
    output_pipe[1] = -1;
    (void)close(diagnostic_pipe[1]);
    diagnostic_pipe[1] = -1;
    free(arguments);

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
            error_set(
                error,
                error_size,
                "fping exited with status %d",
                WEXITSTATUS(status)
            );
        } else if (WIFSIGNALED(status)) {
            error_set(
                error,
                error_size,
                "fping terminated by signal %d",
                WTERMSIG(status)
            );
        } else {
            error_set(error, error_size, "fping stopped unexpectedly");
        }
        return;
    }

    error_set(error, error_size, "fping output closed unexpectedly");
}

void latency_init(struct sqm_mon_latency *latency)
{
    *latency = (struct sqm_mon_latency) {
        .output_descriptor = -1,
        .diagnostic_descriptor = -1,
        .process_identifier = -1,
        .output_buffer = "",
        .output_length = 0U
    };
}

static bool target_is_valid(const char *target)
{
    const char *character;
    size_t length;

    if (target == NULL || target[0] == '\0') {
        return false;
    }
    length = strlen(target);
    if (length >= LATENCY_TARGET_SIZE ||
        !((target[0] >= '0' && target[0] <= '9') ||
            (target[0] >= 'A' && target[0] <= 'Z') ||
            (target[0] >= 'a' && target[0] <= 'z'))) {
        return false;
    }

    for (character = target + 1; *character != '\0'; character++) {
        if (!((*character >= '0' && *character <= '9') ||
                (*character >= 'A' && *character <= 'Z') ||
                (*character >= 'a' && *character <= 'z') ||
                *character == '.' || *character == '-' ||
                *character == '_')) {
            return false;
        }
    }
    return true;
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
    const char *const *targets,
    size_t target_count,
    uint64_t reflector_ping_interval_microseconds,
    char *error,
    size_t error_size
)
{
    size_t index;

    if (interface == NULL || interface[0] == '\0') {
        error_set(error, error_size, "fping interface is empty");
        return -1;
    }
    if (targets == NULL || target_count == 0U) {
        error_set(error, error_size, "fping requires at least one target");
        return -1;
    }
    if (reflector_ping_interval_microseconds / target_count < 1000U) {
        error_set(
            error,
            error_size,
            "reflector ping interval must provide at least 1 ms per target"
        );
        return -1;
    }
    for (index = 0U; index < target_count; index++) {
        if (!target_is_valid(targets[index])) {
            error_set(
                error,
                error_size,
                "latency target '%s' is not a valid IPv4 address or hostname",
                targets[index] == NULL ? "(null)" : targets[index]
            );
            return -1;
        }
    }
    if (start_fping(
            latency,
            interface,
            targets,
            target_count,
            reflector_ping_interval_microseconds,
            error,
            error_size
        ) != 0) {
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

enum latency_probe_result latency_receive(
    struct sqm_mon_latency *latency,
    struct latency_sample *sample,
    char *error,
    size_t error_size
)
{
    if (!latency_is_open(latency)) {
        error_set(error, error_size, "fping is not running");
        return LATENCY_PROBE_ERROR;
    }

    for (;;) {
        {
            char line[LATENCY_OUTPUT_SIZE];
            int line_result = take_output_line(
                latency,
                line,
                sizeof(line)
            );

            if (line_result < 0) {
                error_set(error, error_size, "fping output line is too long");
                return LATENCY_PROBE_ERROR;
            }
            if (line_result > 0) {
                enum latency_fping_line_result parse_result =
                    latency_parse_fping_line(
                        line,
                        sample
                    );

                if (parse_result == LATENCY_FPING_LINE_INVALID) {
                    error_set(
                        error,
                        error_size,
                        "unexpected fping output: %.160s",
                        line
                    );
                    return LATENCY_PROBE_ERROR;
                }
                return parse_result == LATENCY_FPING_LINE_SAMPLE
                    ? LATENCY_PROBE_SUCCESS
                    : LATENCY_PROBE_TIMEOUT;
            }
        }

        if (latency->output_length == sizeof(latency->output_buffer)) {
            error_set(error, error_size, "fping output line is too long");
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
                error_set(
                    error,
                    error_size,
                    "fping reported: %.180s",
                    diagnostic
                );
                return LATENCY_PROBE_ERROR;
            }
            if (received == 0) {
                set_child_exit_error(latency, error, error_size);
                return LATENCY_PROBE_ERROR;
            }
            if (received < 0 && errno == EINTR) {
                continue;
            }
            if (received < 0 && errno != EAGAIN) {
                error_set(
                    error,
                    error_size,
                    "could not read fping diagnostics: %s",
                    strerror(errno)
                );
                return LATENCY_PROBE_ERROR;
            }
        }

        {
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
                set_child_exit_error(latency, error, error_size);
                return LATENCY_PROBE_ERROR;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN) {
                error_set(
                    error,
                    error_size,
                    "could not read fping output: %s",
                    strerror(errno)
                );
                return LATENCY_PROBE_ERROR;
            }
        }
        return LATENCY_PROBE_PENDING;
    }
}
