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
#include <wordexp.h>

#define FPING_PATH "/usr/bin/fping"
#define NULL_PATH "/dev/null"
#define FPING_TIMEOUT_MILLISECONDS "10000"
#define CHILD_STOP_ATTEMPTS 50U
#define CHILD_STOP_INTERVAL_NANOSECONDS 10000000L
#define ALPHA_SCALE 1000000U
#define INITIAL_ONE_WAY_BASELINE_MICROSECONDS 100000U

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

    /* A prefix may launch fping as a child; terminate the owned group too. */
    (void)kill(-process_identifier, SIGTERM);
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

    (void)kill(-process_identifier, SIGKILL);
    while (waitpid(process_identifier, NULL, 0) < 0 && errno == EINTR) {
    }
}

static int spawn_fping(
    pid_t *process_identifier,
    const int output_pipe[2],
    const char *executable,
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
        (result = posix_spawn_file_actions_adddup2(
            &actions,
            output_pipe[1],
            STDOUT_FILENO
        )) != 0 ||
        (result = posix_spawn_file_actions_addclose(
            &actions,
            output_pipe[1]
        )) != 0 ||
        (result = posix_spawn_file_actions_addopen(
            &actions,
            STDERR_FILENO,
            NULL_PATH,
            O_WRONLY,
            0
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
        result = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (result == 0) {
        result = posix_spawnattr_setflags(
            &attributes,
            POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP
        );
    }
    if (result == 0) {
        result = posix_spawnp(
            process_identifier,
            executable,
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
    const char *extra_arguments,
    const char *prefix,
    char *error,
    size_t error_size
)
{
    char period_milliseconds[32];
    char response_interval_milliseconds[32];
    char **arguments;
    wordexp_t extra_words = { 0 };
    wordexp_t prefix_words = { 0 };
    int output_pipe[2] = { -1, -1 };
    pid_t process_identifier;
    uint64_t period;
    uint64_t response_interval;
    size_t index;
    size_t cursor = 0U;
    int word_result;
    bool interface_configured = false;

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

    if (extra_arguments[0] != '\0') {
        word_result = wordexp(extra_arguments, &extra_words, WRDE_NOCMD);
        if (word_result != 0) {
            if (word_result == WRDE_NOSPACE) {
                wordfree(&extra_words);
            }
            error_set(error, error_size, "could not parse ping_extra_args");
            return -1;
        }
    }
    if (prefix[0] != '\0') {
        word_result = wordexp(prefix, &prefix_words, WRDE_NOCMD);
        if (word_result != 0 || prefix_words.we_wordc == 0U) {
            if (word_result == WRDE_NOSPACE || word_result == 0) {
                wordfree(&prefix_words);
            }
            wordfree(&extra_words);
            error_set(error, error_size, "could not parse ping_prefix_string");
            return -1;
        }
    }
    arguments = calloc(
        prefix_words.we_wordc + extra_words.we_wordc + target_count + 13U,
        sizeof(*arguments)
    );
    if (arguments == NULL) {
        error_set(
            error,
            error_size,
            "could not allocate fping arguments: %s",
            strerror(errno)
        );
        goto failed;
    }
    for (index = 0U; index < prefix_words.we_wordc; index++) {
        arguments[cursor++] = prefix_words.we_wordv[index];
    }
    arguments[cursor++] = (char *)FPING_PATH;
    for (index = 0U; index < extra_words.we_wordc; index++) {
        arguments[cursor++] = extra_words.we_wordv[index];
        if (strncmp(extra_words.we_wordv[index], "-I", 2U) == 0 ||
            strcmp(extra_words.we_wordv[index], "--iface") == 0 ||
            strncmp(extra_words.we_wordv[index], "--iface=", 8U) == 0) {
            interface_configured = true;
        }
    }
    /* Keep the SQM interface default, but honor an explicit routing override. */
    if (!interface_configured) {
        arguments[cursor++] = (char *)"-I";
        arguments[cursor++] = (char *)interface;
    }
    arguments[cursor++] = (char *)"--timestamp";
    arguments[cursor++] = (char *)"--loop";
    arguments[cursor++] = (char *)"--period";
    arguments[cursor++] = period_milliseconds;
    arguments[cursor++] = (char *)"--interval";
    arguments[cursor++] = response_interval_milliseconds;
    arguments[cursor++] = (char *)"--timeout";
    arguments[cursor++] = (char *)FPING_TIMEOUT_MILLISECONDS;
    for (index = 0U; index < target_count; index++) {
        arguments[cursor++] = (char *)targets[index];
    }

    if (pipe2(output_pipe, O_CLOEXEC) != 0) {
        error_set(
            error,
            error_size,
            "could not create fping pipe: %s",
            strerror(errno)
        );
        close_pipe(output_pipe);
        free(arguments);
        goto failed;
    }

    if (spawn_fping(
            &process_identifier,
            output_pipe,
            arguments[0],
            arguments,
            error,
            error_size
        ) != 0) {
        close_pipe(output_pipe);
        free(arguments);
        goto failed;
    }

    (void)close(output_pipe[1]);
    output_pipe[1] = -1;
    free(arguments);
    wordfree(&prefix_words);
    wordfree(&extra_words);

    if (set_nonblocking(output_pipe[0], error, error_size) != 0) {
        close_pipe(output_pipe);
        stop_child(process_identifier);
        return -1;
    }

    latency->output_descriptor = output_pipe[0];
    latency->process_identifier = process_identifier;
    latency->output_length = 0U;
    return 0;

failed:
    wordfree(&prefix_words);
    wordfree(&extra_words);
    return -1;
}

static int take_output_line(
    struct sqm_mon_latency *latency,
    char line[LATENCY_OUTPUT_SIZE]
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
    /* The newline occupies a buffer byte, leaving room for the terminator. */
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
        .process_identifier = -1,
        .output_buffer = "",
        .output_length = 0U
    };
}

bool latency_target_is_valid(const char *target)
{
    size_t length;
    static const char allowed[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.:_-";

    if (target == NULL || target[0] == '\0') {
        return false;
    }
    length = strlen(target);
    if (length >= LATENCY_TARGET_SIZE ||
        !(target[0] == ':' || (target[0] >= '0' && target[0] <= '9') ||
            (target[0] >= 'A' && target[0] <= 'Z') ||
            (target[0] >= 'a' && target[0] <= 'z'))) {
        return false;
    }

    return strspn(target, allowed) == length;
}

bool latency_is_open(const struct sqm_mon_latency *latency)
{
    return latency->output_descriptor >= 0 &&
        latency->process_identifier > 0;
}

int latency_open(
    struct sqm_mon_latency *latency,
    const char *interface,
    const char *const *targets,
    size_t target_count,
    uint64_t reflector_ping_interval_microseconds,
    const char *extra_arguments,
    const char *prefix,
    char *error,
    size_t error_size
)
{
    size_t index;

    if (interface == NULL || interface[0] == '\0') {
        error_set(error, error_size, "fping interface is empty");
        return -1;
    }
    if (targets == NULL || target_count == 0U || extra_arguments == NULL ||
        prefix == NULL) {
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
        if (!latency_target_is_valid(targets[index])) {
            error_set(
                error,
                error_size,
                "latency target '%s' is not a valid IP address or hostname",
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
            extra_arguments,
            prefix,
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
    latency->output_descriptor = -1;
    latency->process_identifier = -1;
    latency->output_length = 0U;
    stop_child(process_identifier);
}

int latency_tracker_init(
    struct latency_tracker *tracker,
    const struct latency_tracker_config *config
)
{
    if (config->alpha_baseline_increase_per_million > ALPHA_SCALE ||
        config->alpha_baseline_decrease_per_million > ALPHA_SCALE ||
        config->alpha_delta_ewma_per_million > ALPHA_SCALE) {
        errno = EINVAL;
        return -1;
    }

    tracker->config = *config;
    latency_tracker_reset(tracker);
    return 0;
}

void latency_tracker_reset(struct latency_tracker *tracker)
{
    tracker->one_way_baseline_microseconds =
        INITIAL_ONE_WAY_BASELINE_MICROSECONDS;
    tracker->one_way_delta_ewma_microseconds = 0;
}

void latency_tracker_update(
    struct latency_tracker *tracker,
    const struct latency_sample *sample,
    struct latency_observation *observation
)
{
    uint32_t one_way_microseconds = sample->round_trip_microseconds / 2U;
    uint64_t alpha = one_way_microseconds >=
        tracker->one_way_baseline_microseconds
        ? tracker->config.alpha_baseline_increase_per_million
        : tracker->config.alpha_baseline_decrease_per_million;

    /* This is cake-autorate's integer one-way baseline EWMA. */
    tracker->one_way_baseline_microseconds = (uint32_t)(
        (alpha * one_way_microseconds +
            (ALPHA_SCALE - alpha) *
                tracker->one_way_baseline_microseconds) /
            ALPHA_SCALE
    );

    observation->round_trip_microseconds =
        sample->round_trip_microseconds;
    observation->one_way_microseconds = one_way_microseconds;
    observation->one_way_baseline_microseconds =
        tracker->one_way_baseline_microseconds;
    observation->one_way_delta_microseconds =
        (int64_t)one_way_microseconds -
        (int64_t)tracker->one_way_baseline_microseconds;
    observation->one_way_delta_ewma_microseconds =
        tracker->one_way_delta_ewma_microseconds;
    observation->timestamp_microseconds = sample->timestamp_microseconds;
    observation->sequence = sample->sequence;
}

void latency_tracker_update_delta_ewma(
    struct latency_tracker *tracker,
    bool low_load,
    struct latency_observation *observation
)
{
    int64_t alpha = (int64_t)tracker->config.alpha_delta_ewma_per_million;

    /* cake-autorate freezes reflector delay EWMA while either link is busy. */
    if (low_load) {
        tracker->one_way_delta_ewma_microseconds =
            (alpha * observation->one_way_delta_microseconds +
                ((int64_t)ALPHA_SCALE - alpha) *
                    tracker->one_way_delta_ewma_microseconds) /
            (int64_t)ALPHA_SCALE;
    }
    observation->one_way_delta_ewma_microseconds =
        tracker->one_way_delta_ewma_microseconds;
}

int reflector_health_init(
    struct reflector_health *health,
    const struct reflector_health_config *config,
    uint64_t start_microseconds
)
{
    if (health == NULL || config == NULL || config->detection_window == 0U ||
        config->detection_threshold == 0U ||
        config->detection_threshold > config->detection_window) {
        errno = EINVAL;
        return -1;
    }

    health->offences = calloc(
        config->detection_window,
        sizeof(*health->offences)
    );
    if (health->offences == NULL) {
        return -1;
    }
    health->config = *config;
    health->last_response_microseconds = start_microseconds;
    health->offence_index = 0U;
    health->offence_count = 0U;
    return 0;
}

void reflector_health_cleanup(struct reflector_health *health)
{
    free(health->offences);
    health->offences = NULL;
    health->offence_index = 0U;
    health->offence_count = 0U;
}

void reflector_health_reset(
    struct reflector_health *health,
    uint64_t start_microseconds
)
{
    memset(
        health->offences,
        0,
        health->config.detection_window * sizeof(*health->offences)
    );
    health->last_response_microseconds = start_microseconds;
    health->offence_index = 0U;
    health->offence_count = 0U;
}

void reflector_health_record_response(
    struct reflector_health *health,
    uint64_t timestamp_microseconds
)
{
    health->last_response_microseconds = timestamp_microseconds;
}

enum reflector_health_result reflector_health_check(
    struct reflector_health *health,
    uint64_t timestamp_microseconds
)
{
    bool offence = timestamp_microseconds > health->last_response_microseconds &&
        timestamp_microseconds - health->last_response_microseconds >
            health->config.response_deadline_microseconds;

    if (health->offences[health->offence_index] != 0U) {
        health->offence_count--;
    }
    health->offences[health->offence_index] = offence ? 1U : 0U;
    if (offence) {
        health->offence_count++;
    }
    health->offence_index++;
    if (health->offence_index == health->config.detection_window) {
        health->offence_index = 0U;
    }

    if (health->offence_count >= health->config.detection_threshold) {
        return REFLECTOR_MISBEHAVING;
    }
    return offence ? REFLECTOR_OFFENCE : REFLECTOR_HEALTHY;
}

void reflector_compare(
    const struct latency_tracker *trackers,
    const size_t *reflector_order,
    size_t active_count,
    struct reflector_comparison *comparisons
)
{
    uint64_t minimum_baseline;
    int64_t minimum_delta_ewma;
    size_t index;

    minimum_baseline =
        (uint64_t)trackers[reflector_order[0]].one_way_baseline_microseconds *
        2U;
    minimum_delta_ewma =
        trackers[reflector_order[0]].one_way_delta_ewma_microseconds;
    for (index = 1U; index < active_count; index++) {
        const struct latency_tracker *tracker =
            &trackers[reflector_order[index]];
        uint64_t sum_baselines =
            (uint64_t)tracker->one_way_baseline_microseconds * 2U;

        if (sum_baselines < minimum_baseline) {
            minimum_baseline = sum_baselines;
        }
        if (tracker->one_way_delta_ewma_microseconds < minimum_delta_ewma) {
            minimum_delta_ewma = tracker->one_way_delta_ewma_microseconds;
        }
    }

    for (index = 0U; index < active_count; index++) {
        const struct latency_tracker *tracker =
            &trackers[reflector_order[index]];
        uint64_t sum_baselines =
            (uint64_t)tracker->one_way_baseline_microseconds * 2U;
        int64_t delta_ewma = tracker->one_way_delta_ewma_microseconds;

        comparisons[index] = (struct reflector_comparison) {
            .minimum_sum_owd_baselines_microseconds = minimum_baseline,
            .sum_owd_baselines_microseconds = sum_baselines,
            .sum_owd_baselines_delta_microseconds =
                sum_baselines - minimum_baseline,
            .minimum_download_delta_ewma_microseconds = minimum_delta_ewma,
            .download_delta_ewma_microseconds = delta_ewma,
            .download_delta_ewma_delta_microseconds =
                delta_ewma - minimum_delta_ewma,
            .minimum_upload_delta_ewma_microseconds = minimum_delta_ewma,
            .upload_delta_ewma_microseconds = delta_ewma,
            .upload_delta_ewma_delta_microseconds =
                delta_ewma - minimum_delta_ewma
        };
    }
}

void reflector_rotate(
    size_t *reflector_order,
    size_t reflector_count,
    size_t active_count,
    size_t pinger
)
{
    size_t bad_reflector;

    bad_reflector = reflector_order[pinger];
    reflector_order[pinger] = reflector_order[active_count];
    memmove(
        &reflector_order[active_count],
        &reflector_order[active_count + 1U],
        (reflector_count - active_count - 1U) * sizeof(*reflector_order)
    );
    reflector_order[reflector_count - 1U] = bad_reflector;
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
                line
            );

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
