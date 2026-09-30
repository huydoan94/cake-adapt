#define _GNU_SOURCE

#include "latency.h"
#include "common/error.h"
#include "common/constants.h"
#include "defaults.h"
#include "common/helpers.h"

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

extern char **environ;

static bool parse_timestamp(
    const char *line,
    const char **remainder,
    uint64_t *timestamp_microseconds
)
{
    const char *closing_bracket;
    const char *decimal_point;
    char fraction_digits[7] = FRACTION_ZEROES;
    uint64_t fraction;
    uint64_t seconds;
    size_t digit_count;

    if (line[0] != '[') {
        return false;
    }
    closing_bracket = strchr(line + 1, ']');
    if (
        closing_bracket == NULL ||
        closing_bracket[1] != ' '
    ) {
        return false;
    }
    decimal_point = memchr(
        line + 1,
        '.',
        (size_t)(closing_bracket - (line + 1))
    );
    if (
        decimal_point == NULL ||
        !parse_unsigned(line + 1, decimal_point, &seconds)
    ) {
        return false;
    }

    digit_count = strspn(decimal_point + 1, DECIMAL_DIGITS);
    if (
        digit_count == 0U ||
        decimal_point + 1 + digit_count != closing_bracket
    ) {
        return false;
    }
    /* Pad or truncate to six digits without rounding epoch time through float. */
    memcpy(fraction_digits, decimal_point + 1, digit_count < 6U ? digit_count : 6U);
    fraction = strtoul(fraction_digits, NULL, 10);

    if (seconds > (UINT64_MAX - fraction) / MICROSECONDS_PER_SECOND) {
        return false;
    }
    *timestamp_microseconds = seconds * MICROSECONDS_PER_SECOND + fraction;
    *remainder = closing_bracket + 2;
    return true;
}

enum latency_fping_line_result parse_fping_line(
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

    if (
        line == NULL || sample == NULL ||
        !parse_timestamp(line, &cursor, &timestamp_microseconds)
    ) {
        return LATENCY_FPING_LINE_INVALID;
    }

    separator = strstr(cursor, FPING_SEQUENCE_SEPARATOR);
    if (separator == NULL) {
        return LATENCY_FPING_LINE_INVALID;
    }
    target_end = separator;
    while (target_end > cursor &&
           (target_end[-1] == ' ' || target_end[-1] == '\t')) {
        target_end--;
    }
    target_length = (size_t)(target_end - cursor);
    if (
        target_length == 0U ||
        target_length >= sizeof(sample->target)
    ) {
        return LATENCY_FPING_LINE_INVALID;
    }
    memcpy(sample->target, cursor, target_length);
    sample->target[target_length] = '\0';

    cursor = separator + strlen(FPING_SEQUENCE_SEPARATOR);
    sequence_end = strchr(cursor, ']');
    if (
        sequence_end == NULL ||
        !parse_unsigned(cursor, sequence_end, &sequence)
    ) {
        return LATENCY_FPING_LINE_INVALID;
    }
    sample->download_owd_microseconds = 0U;
    sample->upload_owd_microseconds = 0U;
    sample->timestamp_microseconds = timestamp_microseconds;
    sample->timestamp_rollover_sensitive = false;
    sample->sequence = sequence;
    cursor = sequence_end + 1;
    if (strncmp(cursor, FPING_TIMEOUT_SUFFIX, strlen(FPING_TIMEOUT_SUFFIX)) == 0) {
        return LATENCY_FPING_LINE_TIMEOUT;
    }
    if (strncmp(cursor, FPING_FIELD_SEPARATOR, strlen(FPING_FIELD_SEPARATOR)) != 0) {
        return LATENCY_FPING_LINE_INVALID;
    }

    cursor += strlen(FPING_FIELD_SEPARATOR);
    target_end = cursor;
    cursor += strspn(cursor, DECIMAL_DIGITS);
    if (
        cursor == target_end ||
        strncmp(cursor, FPING_BYTES_SEPARATOR, strlen(FPING_BYTES_SEPARATOR)) != 0
    ) {
        return LATENCY_FPING_LINE_INVALID;
    }
    cursor += strlen(FPING_BYTES_SEPARATOR);
    errno = 0;
    round_trip_milliseconds = strtod(cursor, &rtt_end);
    if (
        errno == ERANGE || rtt_end == cursor ||
        !isfinite(round_trip_milliseconds) ||
        round_trip_milliseconds < 0.0 ||
        strncmp(rtt_end, FPING_MILLISECONDS_SUFFIX, strlen(FPING_MILLISECONDS_SUFFIX)) != 0
    ) {
        return LATENCY_FPING_LINE_INVALID;
    }

    round_trip_microseconds =
        round_trip_milliseconds * (double)MICROSECONDS_PER_MILLISECOND;
    if (round_trip_microseconds > (double)UINT32_MAX) {
        sample->download_owd_microseconds = (int64_t)(UINT32_MAX / 2U);
    } else {
        sample->download_owd_microseconds = (int64_t)(
            (uint32_t)(round_trip_microseconds + 0.5) / 2U
        );
    }
    sample->upload_owd_microseconds = sample->download_owd_microseconds;
    return LATENCY_FPING_LINE_SAMPLE;
}

static bool token_has_unit(const char *token, const char *unit)
{
    size_t length = strlen(unit);

    return strncmp(token, unit, length) == 0 &&
        (token[length] == '\0' || token[length] == ' ' || token[length] == '\t');
}

static bool parse_irtt_duration(
    const char *value,
    int64_t *microseconds
)
{
    char *unit;
    double parsed;
    double scale;
    double converted;

    errno = 0;
    parsed = strtod(value, &unit);
    if (
        errno == ERANGE || unit == value ||
        !isfinite(parsed) || parsed < 0.0
    ) {
        return false;
    }
    if (token_has_unit(unit, "ns")) {
        scale = 1.0 / (double)THOUSAND;
    } else if (
        token_has_unit(unit, "us") ||
        token_has_unit(unit, "µs")
    ) {
        scale = (double)MICROSECOND;
    } else if (token_has_unit(unit, "ms")) {
        scale = (double)MILLISECOND;
    } else if (token_has_unit(unit, "s")) {
        scale = (double)SECOND;
    } else {
        return false;
    }
    converted = parsed * scale;
    if (!isfinite(converted) || converted >= (double)INT64_MAX) {
        return false;
    }
    *microseconds = (int64_t)(converted + 0.5);
    return true;
}

static bool irtt_value(
    const char *line,
    const char *name,
    const char **value
)
{
    size_t name_length = strlen(name);
    const char *cursor = line;

    while (*cursor != '\0') {
        const char *end = strpbrk(cursor, " \t");
        size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);

        if (length > name_length &&
            strncmp(cursor, name, name_length) == 0 &&
            cursor[name_length] == '=') {
            *value = cursor + name_length + 1U;
            return true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + strspn(end, " \t");
    }
    return false;
}

bool parse_irtt_line(
    const char *line,
    const char *target,
    uint64_t timestamp_microseconds,
    struct latency_sample *sample
)
{
    const char *sequence_text;
    const char *download_text;
    const char *upload_text;
    char *sequence_end;
    uintmax_t sequence;
    int64_t download;
    int64_t upload;

    if (
        line == NULL || target == NULL || sample == NULL ||
        !irtt_value(line, "seq", &sequence_text) ||
        !irtt_value(line, "rd", &download_text) ||
        !irtt_value(line, "sd", &upload_text)
    ) {
        return false;
    }
    errno = 0;
    sequence = strtoumax(sequence_text, &sequence_end, 10);
    if (
        errno == ERANGE || sequence_end == sequence_text ||
        (*sequence_end != '\0' && *sequence_end != ' ' && *sequence_end != '\t') ||
        !parse_irtt_duration(download_text, &download) ||
        !parse_irtt_duration(upload_text, &upload)
    ) {
        return false;
    }
    if (strlen(target) >= sizeof(sample->target)) {
        return false;
    }
    *sample = (struct latency_sample) {
        .download_owd_microseconds = download,
        .upload_owd_microseconds = upload,
        .timestamp_microseconds = timestamp_microseconds,
        .timestamp_rollover_sensitive = false,
        .sequence = (uint64_t)sequence
    };
    (void)strcpy(sample->target, target);
    return true;
}

static int set_nonblocking(
    int descriptor,
    const char *name,
    char *error,
    size_t error_size
)
{
    int flags = fcntl(descriptor, F_GETFL);
    if (
        flags < 0 ||
        fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0
    ) {
        error_set(
            error,
            error_size,
            "could not make %s pipe nonblocking: %s",
            name,
            strerror(errno)
        );
        return -1;
    }
    return 0;
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

        if (
            result == process_identifier ||
            (result < 0 && errno == ECHILD)
        ) {
            return;
        }
        if (
            result < 0 &&
            errno != EINTR
        ) {
            return;
        }
        (void)nanosleep(&interval, NULL);
    }

    (void)kill(-process_identifier, SIGKILL);
    while (waitpid(process_identifier, NULL, 0) < 0 && errno == EINTR) {
    }
}

static int spawn_child(
    pid_t *process_identifier,
    const int output_pipe[2],
    const char *executable,
    char *const arguments[],
    const char *name,
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
    if (
        (result = posix_spawn_file_actions_addclose(
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
            NULL_DEVICE_PATH,
            O_WRONLY,
            0
        )) != 0
    ) {
        goto destroy_actions;
    }

    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        goto destroy_actions;
    }
    (void)sigemptyset(&child_signal_mask);

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

    (void)posix_spawnattr_destroy(&attributes);
destroy_actions:
    (void)posix_spawn_file_actions_destroy(&actions);
failed:
    if (result != 0) {
        error_set(
            error,
            error_size,
            "could not start %s: %s",
            name,
            strerror(result)
        );
        return -1;
    }
    return 0;
}

int latency_open(
    struct latency *latency,
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
    int output_pipe[2];
    pid_t process_identifier;
    uint64_t period;
    uint64_t response_interval;
    size_t index;
    size_t cursor = 0U;
    int word_result;
    bool interface_configured = false;

    if (
        interface == NULL ||
        interface[0] == '\0'
    ) {
        error_set(error, error_size, "fping interface is empty");
        return -1;
    }
    if (
        targets == NULL || target_count == 0U || extra_arguments == NULL ||
        prefix == NULL
    ) {
        error_set(error, error_size, "fping requires at least one target");
        return -1;
    }
    if (target_count > CONFIG_MAX_REFLECTORS) {
        error_set(
            error,
            error_size,
            "fping supports at most %u targets",
            CONFIG_MAX_REFLECTORS
        );
        return -1;
    }
    if (
        reflector_ping_interval_microseconds / target_count <
        MICROSECONDS_PER_MILLISECOND
    ) {
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
                "latency target '%s' is not a valid IP address or hostname",
                targets[index] == NULL ? NULL_VALUE : targets[index]
            );
            return -1;
        }
    }

    period = rounded_divide(
        reflector_ping_interval_microseconds,
        MICROSECONDS_PER_MILLISECOND
    );
    response_interval =
        reflector_ping_interval_microseconds / target_count /
        MICROSECONDS_PER_MILLISECOND;
    (void)snprintf(
        period_milliseconds,
        sizeof(period_milliseconds),
        "%" PRIu64,
        period
    );
    (void)snprintf(
        response_interval_milliseconds,
        sizeof(response_interval_milliseconds),
        "%" PRIu64,
        response_interval
    );

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
        if (
            word_result != 0 ||
            prefix_words.we_wordc == 0U
        ) {
            if (
                word_result == WRDE_NOSPACE ||
                word_result == 0
            ) {
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
        if (
            strncmp(
                extra_words.we_wordv[index],
                FPING_INTERFACE_SHORT,
                strlen(FPING_INTERFACE_SHORT)
            ) == 0 ||
            strcmp(extra_words.we_wordv[index], FPING_INTERFACE_LONG) == 0 ||
            strncmp(
                extra_words.we_wordv[index],
                FPING_INTERFACE_LONG_PREFIX,
                strlen(FPING_INTERFACE_LONG_PREFIX)
            ) == 0
        ) {
            interface_configured = true;
        }
    }
    /* Keep the SQM interface default, but honor an explicit routing override. */
    if (!interface_configured) {
        arguments[cursor++] = (char *)FPING_INTERFACE_SHORT;
        arguments[cursor++] = (char *)interface;
    }
    arguments[cursor++] = (char *)FPING_TIMESTAMP;
    arguments[cursor++] = (char *)FPING_LOOP;
    arguments[cursor++] = (char *)FPING_PERIOD;
    arguments[cursor++] = period_milliseconds;
    arguments[cursor++] = (char *)FPING_INTERVAL;
    arguments[cursor++] = response_interval_milliseconds;
    arguments[cursor++] = (char *)FPING_TIMEOUT;
    arguments[cursor++] = (char *)DEFAULT_FPING_TIMEOUT_MILLISECONDS;
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
        goto free_arguments;
    }

    if (
        spawn_child(
            &process_identifier,
            output_pipe,
            arguments[0],
            arguments,
            PINGER_METHOD_FPING,
            error,
            error_size
        ) != 0
    ) {
        goto close_output;
    }

    (void)close(output_pipe[1]);
    free(arguments);
    wordfree(&prefix_words);
    wordfree(&extra_words);

    if (set_nonblocking(
        output_pipe[0],
        PINGER_METHOD_FPING,
        error,
        error_size
    ) != 0) {
        (void)close(output_pipe[0]);
        stop_child(process_identifier);
        return -1;
    }

    latency->children[0].output_descriptor = output_pipe[0];
    latency->children[0].process_identifier = process_identifier;
    latency->children[0].output_length = 0U;
    latency->backend = LATENCY_BACKEND_FPING;
    latency->active = true;
    latency->child_count = 1U;
    return 0;

close_output:
    (void)close(output_pipe[0]);
    (void)close(output_pipe[1]);
free_arguments:
    free(arguments);
failed:
    wordfree(&prefix_words);
    wordfree(&extra_words);
    return -1;
}

static int expand_words(
    const char *value,
    bool require_word,
    const char *option,
    wordexp_t *words,
    char *error,
    size_t error_size
)
{
    int result;

    if (value[0] == '\0') {
        return 0;
    }
    result = wordexp(value, words, WRDE_NOCMD);
    if (result == 0 && (!require_word || words->we_wordc > 0U)) {
        return 0;
    }
    if (result == WRDE_NOSPACE || result == 0) {
        wordfree(words);
        *words = (wordexp_t) { 0 };
    }
    error_set(error, error_size, "could not parse %s", option);
    return -1;
}

static int spawn_irtt_child(
    struct latency *latency,
    size_t child_index,
    char *error,
    size_t error_size
)
{
    struct latency_child *child = &latency->children[child_index];
    char interval[32];
    char duration[32];
    char endpoint[LATENCY_TARGET_SIZE + 3U];
    char **arguments = NULL;
    wordexp_t extra_words = { 0 };
    wordexp_t prefix_words = { 0 };
    int output_pipe[2] = { -1, -1 };
    pid_t process_identifier;
    size_t cursor = 0U;
    size_t index;
    int result = -1;

    if (
        expand_words(
            latency->ping_extra_args,
            false,
            OPTION_PING_EXTRA_ARGS,
            &extra_words,
            error,
            error_size
        ) != 0 ||
        expand_words(
            latency->ping_prefix_string,
            true,
            OPTION_PING_PREFIX_STRING,
            &prefix_words,
            error,
            error_size
        ) != 0
    ) {
        goto done;
    }

    (void)snprintf(
        interval,
        sizeof(interval),
        "%" PRIu64 ".%06" PRIu64 "s",
        latency->reflector_ping_interval_microseconds / SECOND,
        latency->reflector_ping_interval_microseconds % SECOND
    );
    (void)snprintf(
        duration,
        sizeof(duration),
        "%" PRIu64 "m",
        latency->irtt_session_duration_minutes
    );
    if (strchr(child->target, ':') == NULL) {
        (void)strcpy(endpoint, child->target);
    } else {
        (void)snprintf(endpoint, sizeof(endpoint), "[%s]", child->target);
    }

    arguments = calloc(
        prefix_words.we_wordc + extra_words.we_wordc + 8U,
        sizeof(*arguments)
    );
    if (arguments == NULL) {
        error_set(
            error,
            error_size,
            "could not allocate irtt arguments: %s",
            strerror(errno)
        );
        goto done;
    }
    for (index = 0U; index < prefix_words.we_wordc; index++) {
        arguments[cursor++] = prefix_words.we_wordv[index];
    }
    arguments[cursor++] = (char *)IRTT_PATH;
    arguments[cursor++] = (char *)IRTT_CLIENT;
    for (index = 0U; index < extra_words.we_wordc; index++) {
        arguments[cursor++] = extra_words.we_wordv[index];
    }
    arguments[cursor++] = (char *)IRTT_INTERVAL;
    arguments[cursor++] = interval;
    arguments[cursor++] = (char *)IRTT_DURATION;
    arguments[cursor++] = duration;
    arguments[cursor++] = endpoint;

    if (pipe2(output_pipe, O_CLOEXEC) != 0) {
        error_set(
            error,
            error_size,
            "could not create irtt pipe: %s",
            strerror(errno)
        );
        goto done;
    }
    if (
        spawn_child(
            &process_identifier,
            output_pipe,
            arguments[0],
            arguments,
            PINGER_METHOD_IRTT,
            error,
            error_size
        ) != 0
    ) {
        goto done;
    }
    (void)close(output_pipe[1]);
    output_pipe[1] = -1;
    if (set_nonblocking(
        output_pipe[0],
        PINGER_METHOD_IRTT,
        error,
        error_size
    ) != 0) {
        stop_child(process_identifier);
        goto done;
    }

    child->output_descriptor = output_pipe[0];
    child->process_identifier = process_identifier;
    child->output_length = 0U;
    output_pipe[0] = -1;
    result = 0;

done:
    if (output_pipe[0] >= 0) {
        (void)close(output_pipe[0]);
    }
    if (output_pipe[1] >= 0) {
        (void)close(output_pipe[1]);
    }
    free(arguments);
    wordfree(&prefix_words);
    wordfree(&extra_words);
    return result;
}

int latency_open_irtt(
    struct latency *latency,
    const char *const *targets,
    size_t target_count,
    uint64_t reflector_ping_interval_microseconds,
    uint64_t session_duration_minutes,
    const char *extra_arguments,
    const char *prefix,
    uint64_t first_start_microseconds,
    char *error,
    size_t error_size
)
{
    wordexp_t words = { 0 };
    uint64_t child_start_spacing_microseconds;
    size_t index;

    if (
        targets == NULL || target_count == 0U ||
        target_count > CONFIG_MAX_REFLECTORS ||
        reflector_ping_interval_microseconds == 0U ||
        reflector_ping_interval_microseconds / target_count < MILLISECOND ||
        session_duration_minutes == 0U ||
        extra_arguments == NULL || prefix == NULL
    ) {
        error_set(error, error_size, "invalid irtt session configuration");
        return -1;
    }
    if (
        expand_words(
            extra_arguments,
            false,
            OPTION_PING_EXTRA_ARGS,
            &words,
            error,
            error_size
        ) != 0
    ) {
        return -1;
    }
    wordfree(&words);
    words = (wordexp_t) { 0 };
    if (
        expand_words(
            prefix,
            true,
            OPTION_PING_PREFIX_STRING,
            &words,
            error,
            error_size
        ) != 0
    ) {
        return -1;
    }
    wordfree(&words);

    child_start_spacing_microseconds =
        reflector_ping_interval_microseconds / target_count;
    for (index = 0U; index < target_count; index++) {
        if (!target_is_valid(targets[index])) {
            error_set(
                error,
                error_size,
                "latency target '%s' is not a valid IP address or hostname",
                targets[index] == NULL ? NULL_VALUE : targets[index]
            );
            return -1;
        }
        latency->children[index].target = targets[index];
        latency->children[index].next_start_microseconds =
            first_start_microseconds +
            index * child_start_spacing_microseconds;
    }
    latency->backend = LATENCY_BACKEND_IRTT;
    latency->active = true;
    latency->irtt_session_duration_minutes = session_duration_minutes;
    latency->reflector_ping_interval_microseconds =
        reflector_ping_interval_microseconds;
    latency->ping_extra_args = extra_arguments;
    latency->ping_prefix_string = prefix;
    latency->child_count = target_count;
    return 0;
}

int latency_start_irtt_children(
    struct latency *latency,
    uint64_t timestamp_microseconds,
    char *error,
    size_t error_size
)
{
    size_t index;

    if (!latency->active || latency->backend != LATENCY_BACKEND_IRTT) {
        error_set(error, error_size, "irtt session is not active");
        return -1;
    }
    for (index = 0U; index < latency->child_count; index++) {
        struct latency_child *child = &latency->children[index];

        if (
            child->output_descriptor >= 0 ||
            timestamp_microseconds < child->next_start_microseconds
        ) {
            continue;
        }
        if (
            spawn_irtt_child(
                latency,
                index,
                error,
                error_size
            ) != 0
        ) {
            latency_close(latency);
            return -1;
        }
        child->started_microseconds = timestamp_microseconds;
    }
    return 0;
}

bool latency_irtt_start_pending(const struct latency *latency)
{
    size_t index;

    if (!latency->active || latency->backend != LATENCY_BACKEND_IRTT) {
        return false;
    }
    for (index = 0U; index < latency->child_count; index++) {
        if (latency->children[index].output_descriptor < 0) {
            return true;
        }
    }
    return false;
}

uint64_t latency_irtt_next_start_microseconds(const struct latency *latency)
{
    uint64_t next = UINT64_MAX;
    size_t index;

    for (index = 0U; index < latency->child_count; index++) {
        const struct latency_child *child = &latency->children[index];

        if (
            child->output_descriptor < 0 &&
            child->next_start_microseconds < next
        ) {
            next = child->next_start_microseconds;
        }
    }
    return next;
}

static bool take_output_line(
    struct latency_child *child,
    char line[LATENCY_OUTPUT_SIZE]
)
{
    char *newline = memchr(
        child->output_buffer,
        '\n',
        child->output_length
    );
    size_t length;
    size_t consumed;

    if (newline == NULL) {
        return false;
    }

    length = (size_t)(newline - child->output_buffer);
    consumed = length + 1U;
    if (
        length > 0U &&
        child->output_buffer[length - 1U] == '\r'
    ) {
        --length;
    }
    /* The newline occupies a buffer byte, leaving room for the terminator. */
    memcpy(line, child->output_buffer, length);
    line[length] = '\0';
    memmove(
        child->output_buffer,
        child->output_buffer + consumed,
        child->output_length - consumed
    );
    child->output_length -= consumed;
    return true;
}

static void set_child_exit_error(
    struct latency_child *child,
    const char *name,
    char *error,
    size_t error_size
)
{
    int status;
    pid_t result;

    result = waitpid(child->process_identifier, &status, WNOHANG);
    if (result == child->process_identifier) {
        child->process_identifier = -1;
        if (WIFEXITED(status)) {
            error_set(
                error,
                error_size,
                "%s exited with status %d",
                name,
                WEXITSTATUS(status)
            );
        } else if (WIFSIGNALED(status)) {
            error_set(
                error,
                error_size,
                "%s terminated by signal %d",
                name,
                WTERMSIG(status)
            );
        } else {
            error_set(error, error_size, "%s stopped unexpectedly", name);
        }
        return;
    }

    error_set(error, error_size, "%s output closed unexpectedly", name);
}

static enum latency_probe_result schedule_irtt_restart(
    struct latency_child *child,
    char *error,
    size_t error_size
)
{
    uint64_t timestamp_microseconds;
    uint64_t runtime_microseconds;

    if (!read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
        error_set(
            error,
            error_size,
            "could not schedule irtt restart: %s",
            strerror(errno)
        );
        return LATENCY_PROBE_ERROR;
    }
    set_child_exit_error(child, PINGER_METHOD_IRTT, error, error_size);
    stop_child(child->process_identifier);
    child->process_identifier = -1;
    if (child->output_descriptor >= 0) {
        (void)close(child->output_descriptor);
    }
    child->output_descriptor = -1;
    child->output_length = 0U;
    runtime_microseconds = timestamp_microseconds >= child->started_microseconds
        ? timestamp_microseconds - child->started_microseconds
        : 0U;
    child->next_start_microseconds = timestamp_microseconds;
    if (runtime_microseconds < IRTT_FAST_EXIT_THRESHOLD_MICROSECONDS) {
        child->next_start_microseconds += IRTT_FAST_EXIT_RETRY_MICROSECONDS;
    }
    return LATENCY_PROBE_RESTART;
}

void latency_init(struct latency *latency)
{
    size_t index;

    *latency = (struct latency) { 0 };
    for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
        latency->children[index].output_descriptor = -1;
        latency->children[index].process_identifier = -1;
    }
}

bool target_is_valid(const char *target)
{
    size_t length;

    if (
        target == NULL ||
        target[0] == '\0'
    ) {
        return false;
    }
    length = strlen(target);
    if (
        length >= LATENCY_TARGET_SIZE ||
        !(target[0] == ':' || (target[0] >= '0' && target[0] <= '9') ||
            (target[0] >= 'A' && target[0] <= 'Z') ||
            (target[0] >= 'a' && target[0] <= 'z'))
    ) {
        return false;
    }

    return strspn(target, TARGET_CHARACTERS) == length;
}

bool latency_is_open(const struct latency *latency)
{
    return latency->active;
}

size_t latency_child_count(const struct latency *latency)
{
    return latency->child_count;
}

int latency_child_descriptor(const struct latency *latency, size_t child_index)
{
    if (child_index >= latency->child_count) {
        return -1;
    }
    return latency->children[child_index].output_descriptor;
}

void latency_close(struct latency *latency)
{
    size_t index;

    for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
        struct latency_child *child = &latency->children[index];
        pid_t process_identifier = child->process_identifier;

        if (child->output_descriptor >= 0) {
            (void)close(child->output_descriptor);
        }
        child->output_descriptor = -1;
        child->process_identifier = -1;
        child->target = NULL;
        child->output_length = 0U;
        stop_child(process_identifier);
    }
    latency->active = false;
    latency->ping_extra_args = NULL;
    latency->ping_prefix_string = NULL;
    latency->child_count = 0U;
}

int tracker_init(
    struct latency_tracker *tracker,
    const struct latency_tracker_config *config
)
{
    if (
        config->alpha_baseline_increase_per_million > MILLION ||
        config->alpha_baseline_decrease_per_million > MILLION ||
        config->alpha_delta_ewma_per_million > MILLION
    ) {
        errno = EINVAL;
        return -1;
    }

    tracker->config = *config;
    tracker_reset(tracker);
    return 0;
}

void tracker_reset(struct latency_tracker *tracker)
{
    tracker->download.baseline_microseconds =
        (int64_t)INITIAL_ONE_WAY_BASELINE_MICROSECONDS;
    tracker->download.delta_ewma_microseconds = 0;
    tracker->upload = tracker->download;
}

static uint64_t absolute_difference(int64_t first, int64_t second)
{
    if (first >= second) {
        return (uint64_t)first - (uint64_t)second;
    }
    return (uint64_t)second - (uint64_t)first;
}

static int64_t signed_difference(int64_t first, int64_t second)
{
    uint64_t difference = absolute_difference(first, second);

    if (first >= second) {
        return difference > (uint64_t)INT64_MAX
            ? INT64_MAX
            : (int64_t)difference;
    }
    return difference > (uint64_t)INT64_MAX
        ? INT64_MIN
        : -(int64_t)difference;
}

static int64_t signed_sum(int64_t first, int64_t second)
{
    int64_t sum;

    if (!__builtin_add_overflow(first, second, &sum)) {
        return sum;
    }
    return first < 0 ? INT64_MIN : INT64_MAX;
}

static bool sample_has_timestamp_rollover(
    const struct latency_tracker *tracker,
    const struct latency_sample *sample
)
{
    uint64_t download_delta;
    uint64_t upload_delta;

    if (!sample->timestamp_rollover_sensitive) {
        return false;
    }
    download_delta = absolute_difference(
        sample->download_owd_microseconds,
        tracker->download.baseline_microseconds
    );
    upload_delta = absolute_difference(
        sample->upload_owd_microseconds,
        tracker->upload.baseline_microseconds
    );
    return download_delta >= LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS ||
        upload_delta >= LATENCY_TIMESTAMP_ROLLOVER_DELTA_MICROSECONDS -
            download_delta;
}

static bool weighted_average(
    int64_t alpha,
    int64_t value_microseconds,
    int64_t baseline_microseconds,
    int64_t *average_microseconds
)
{
    int64_t value_component;
    int64_t baseline_component;
    int64_t weighted;

    if (
        __builtin_mul_overflow(alpha, value_microseconds, &value_component) ||
        __builtin_mul_overflow(
            (int64_t)MILLION - alpha,
            baseline_microseconds,
            &baseline_component
        ) ||
        __builtin_add_overflow(value_component, baseline_component, &weighted)
    ) {
        return false;
    }
    *average_microseconds = weighted / (int64_t)MILLION;
    return true;
}

static void tracker_update_direction(
    const struct latency_tracker_config *config,
    struct latency_direction_tracker *state,
    int64_t value_microseconds,
    int64_t *baseline_microseconds,
    int64_t *delta_microseconds
)
{
    int64_t alpha = value_microseconds >= state->baseline_microseconds
        ? (int64_t)config->alpha_baseline_increase_per_million
        : (int64_t)config->alpha_baseline_decrease_per_million;

    if (!weighted_average(
        alpha,
        value_microseconds,
        state->baseline_microseconds,
        &state->baseline_microseconds
    )) {
        /* Extreme timestamp values cannot preserve a meaningful EWMA. */
        state->baseline_microseconds = value_microseconds;
    }

    *baseline_microseconds = state->baseline_microseconds;
    *delta_microseconds =
        signed_difference(value_microseconds, state->baseline_microseconds);
}

void tracker_update(
    struct latency_tracker *tracker,
    const struct latency_sample *sample,
    struct latency_observation *observation
)
{
    if (sample_has_timestamp_rollover(tracker, sample)) {
        tracker->download.baseline_microseconds =
            sample->download_owd_microseconds;
        tracker->upload.baseline_microseconds = sample->upload_owd_microseconds;
        observation->download_owd_microseconds =
            sample->download_owd_microseconds;
        observation->download_owd_baseline_microseconds =
            sample->download_owd_microseconds;
        observation->download_owd_delta_microseconds = 0;
        observation->upload_owd_microseconds = sample->upload_owd_microseconds;
        observation->upload_owd_baseline_microseconds =
            sample->upload_owd_microseconds;
        observation->upload_owd_delta_microseconds = 0;
        observation->download_owd_delta_ewma_microseconds =
            tracker->download.delta_ewma_microseconds;
        observation->upload_owd_delta_ewma_microseconds =
            tracker->upload.delta_ewma_microseconds;
        observation->timestamp_microseconds = sample->timestamp_microseconds;
        observation->sequence = sample->sequence;
        return;
    }
    observation->download_owd_microseconds = sample->download_owd_microseconds;
    observation->upload_owd_microseconds = sample->upload_owd_microseconds;
    tracker_update_direction(
        &tracker->config,
        &tracker->download,
        sample->download_owd_microseconds,
        &observation->download_owd_baseline_microseconds,
        &observation->download_owd_delta_microseconds
    );
    tracker_update_direction(
        &tracker->config,
        &tracker->upload,
        sample->upload_owd_microseconds,
        &observation->upload_owd_baseline_microseconds,
        &observation->upload_owd_delta_microseconds
    );
    observation->download_owd_delta_ewma_microseconds =
        tracker->download.delta_ewma_microseconds;
    observation->upload_owd_delta_ewma_microseconds =
        tracker->upload.delta_ewma_microseconds;
    observation->timestamp_microseconds = sample->timestamp_microseconds;
    observation->sequence = sample->sequence;
}

void tracker_update_delta_ewma(
    struct latency_tracker *tracker,
    bool low_load,
    struct latency_observation *observation
)
{
    int64_t alpha = (int64_t)tracker->config.alpha_delta_ewma_per_million;

    /* cake-autorate freezes reflector delay EWMA while either link is busy. */
    if (low_load) {
        tracker->download.delta_ewma_microseconds =
            (alpha * observation->download_owd_delta_microseconds +
                ((int64_t)MILLION - alpha) *
                    tracker->download.delta_ewma_microseconds) /
            (int64_t)MILLION;
        tracker->upload.delta_ewma_microseconds =
            (alpha * observation->upload_owd_delta_microseconds +
                ((int64_t)MILLION - alpha) *
                    tracker->upload.delta_ewma_microseconds) /
            (int64_t)MILLION;
    }
    observation->download_owd_delta_ewma_microseconds =
        tracker->download.delta_ewma_microseconds;
    observation->upload_owd_delta_ewma_microseconds =
        tracker->upload.delta_ewma_microseconds;
}

int health_init(
    struct reflector_health *health,
    const struct reflector_health_config *config,
    uint64_t start_microseconds
)
{
    if (
        health == NULL || config == NULL || config->detection_window == 0U ||
        config->detection_threshold == 0U ||
        config->detection_threshold > config->detection_window
    ) {
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

void health_cleanup(struct reflector_health *health)
{
    free(health->offences);
    health->offences = NULL;
    health->offence_index = 0U;
    health->offence_count = 0U;
}

void health_reset(
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

void health_record_response(
    struct reflector_health *health,
    uint64_t timestamp_microseconds
)
{
    health->last_response_microseconds = timestamp_microseconds;
}

enum reflector_health_result health_check(
    struct reflector_health *health,
    uint64_t timestamp_microseconds
)
{
    bool offence = interval_elapsed(
        timestamp_microseconds,
        health->last_response_microseconds,
        health->config.response_deadline_microseconds
    );

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
    int64_t minimum_baseline;
    int64_t minimum_download_delta_ewma;
    int64_t minimum_upload_delta_ewma;
    size_t index;

    minimum_baseline = signed_sum(
        trackers[reflector_order[0]].download.baseline_microseconds,
        trackers[reflector_order[0]].upload.baseline_microseconds
    );
    minimum_download_delta_ewma =
        trackers[reflector_order[0]].download.delta_ewma_microseconds;
    minimum_upload_delta_ewma =
        trackers[reflector_order[0]].upload.delta_ewma_microseconds;
    for (index = 1U; index < active_count; index++) {
        const struct latency_tracker *tracker =
            &trackers[reflector_order[index]];
        int64_t sum_baselines = signed_sum(
            tracker->download.baseline_microseconds,
            tracker->upload.baseline_microseconds
        );

        if (sum_baselines < minimum_baseline) {
            minimum_baseline = sum_baselines;
        }
        if (
            tracker->download.delta_ewma_microseconds <
            minimum_download_delta_ewma
        ) {
            minimum_download_delta_ewma =
                tracker->download.delta_ewma_microseconds;
        }
        if (
            tracker->upload.delta_ewma_microseconds <
            minimum_upload_delta_ewma
        ) {
            minimum_upload_delta_ewma =
                tracker->upload.delta_ewma_microseconds;
        }
    }

    for (index = 0U; index < active_count; index++) {
        const struct latency_tracker *tracker =
            &trackers[reflector_order[index]];
        int64_t sum_baselines = signed_sum(
            tracker->download.baseline_microseconds,
            tracker->upload.baseline_microseconds
        );
        int64_t download_delta_ewma =
            tracker->download.delta_ewma_microseconds;
        int64_t upload_delta_ewma =
            tracker->upload.delta_ewma_microseconds;

        comparisons[index] = (struct reflector_comparison) {
            .minimum_sum_owd_baselines_microseconds = minimum_baseline,
            .sum_owd_baselines_microseconds = sum_baselines,
            .sum_owd_baselines_delta_microseconds =
                absolute_difference(sum_baselines, minimum_baseline),
            .minimum_download_delta_ewma_microseconds =
                minimum_download_delta_ewma,
            .download_delta_ewma_microseconds = download_delta_ewma,
            .download_delta_ewma_delta_microseconds =
                signed_difference(
                    download_delta_ewma,
                    minimum_download_delta_ewma
                ),
            .minimum_upload_delta_ewma_microseconds =
                minimum_upload_delta_ewma,
            .upload_delta_ewma_microseconds = upload_delta_ewma,
            .upload_delta_ewma_delta_microseconds =
                signed_difference(
                    upload_delta_ewma,
                    minimum_upload_delta_ewma
                )
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

enum latency_probe_result latency_receive_child(
    struct latency *latency,
    size_t child_index,
    struct latency_sample *sample,
    char *error,
    size_t error_size
)
{
    struct latency_child *child;
    const char *name = latency->backend == LATENCY_BACKEND_IRTT
        ? PINGER_METHOD_IRTT
        : PINGER_METHOD_FPING;

    if (child_index >= latency->child_count) {
        error_set(error, error_size, "%s is not running", name);
        return LATENCY_PROBE_ERROR;
    }
    child = &latency->children[child_index];
    if (child->output_descriptor < 0 || child->process_identifier <= 0) {
        error_set(error, error_size, "%s is not running", name);
        return LATENCY_PROBE_ERROR;
    }

    for (;;) {
        char line[LATENCY_OUTPUT_SIZE];
        ssize_t received;

        if (take_output_line(child, line)) {
            enum latency_fping_line_result parsed;

            if (latency->backend == LATENCY_BACKEND_IRTT) {
                uint64_t timestamp_microseconds;

                if (!parse_irtt_line(line, child->target, 0U, sample)) {
                    continue;
                }
                if (!read_clock_microseconds(
                    CLOCK_REALTIME,
                    &timestamp_microseconds
                )) {
                    error_set(
                        error,
                        error_size,
                        "could not timestamp irtt output: %s",
                        strerror(errno)
                    );
                    return LATENCY_PROBE_ERROR;
                }
                sample->timestamp_microseconds = timestamp_microseconds;
                return LATENCY_PROBE_SUCCESS;
            }
            parsed = parse_fping_line(line, sample);
            if (parsed == LATENCY_FPING_LINE_INVALID) {
                error_set(
                    error,
                    error_size,
                    "unexpected fping output: %.160s",
                    line
                );
                return LATENCY_PROBE_ERROR;
            }
            return parsed == LATENCY_FPING_LINE_SAMPLE
                ? LATENCY_PROBE_SUCCESS
                : LATENCY_PROBE_TIMEOUT;
        }

        if (child->output_length == sizeof(child->output_buffer)) {
            error_set(error, error_size, "%s output line is too long", name);
            return LATENCY_PROBE_ERROR;
        }

        received = read(
            child->output_descriptor,
            child->output_buffer + child->output_length,
            sizeof(child->output_buffer) - child->output_length
        );
        if (received > 0) {
            child->output_length += (size_t)received;
            continue;
        }
        if (received == 0) {
            if (latency->backend == LATENCY_BACKEND_IRTT) {
                return schedule_irtt_restart(child, error, error_size);
            }
            set_child_exit_error(child, name, error, error_size);
            return LATENCY_PROBE_ERROR;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN) {
            error_set(
                error,
                error_size,
                "could not read %s output: %s",
                name,
                strerror(errno)
            );
            return LATENCY_PROBE_ERROR;
        }
        return LATENCY_PROBE_PENDING;
    }
}
