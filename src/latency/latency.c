#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/pinger.h"
#include "common/error.h"
#include "common/constants.h"
#include "config/defaults.h"
#include "common/helpers.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

int set_nonblocking(
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

void stop_child(pid_t process_identifier)
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

int spawn_child(
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

void set_child_exit_error(
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
