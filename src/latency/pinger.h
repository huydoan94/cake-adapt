#ifndef LATENCY_PINGER_H_INCLUDED
#define LATENCY_PINGER_H_INCLUDED

/* Private interface between latency session ownership and pinger backends. */

#include <stddef.h>
#include <sys/types.h>

#include "latency/latency.h"

int set_nonblocking(
    int descriptor,
    const char *name,
    char *error,
    size_t error_size
);

/* Terminates the child's process group, escalating to SIGKILL, and reaps it. */
void stop_child(pid_t process_identifier);

/* stdout goes to output_pipe[1], stderr to /dev/null, in a new process group. */
int spawn_child(
    pid_t *process_identifier,
    const int output_pipe[2],
    const char *executable,
    char *const arguments[],
    const char *name,
    char *error,
    size_t error_size
);

void set_child_exit_error(
    struct latency_child *child,
    const char *name,
    char *error,
    size_t error_size
);

/* Reap an ended IRTT session and schedule its restart, delayed after a fast exit. */
enum latency_probe_result schedule_irtt_restart(
    struct latency_child *child,
    char *error,
    size_t error_size
);

#endif
