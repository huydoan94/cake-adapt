#ifndef TCPDELAY_CAPTURE_H_INCLUDED
#define TCPDELAY_CAPTURE_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "tcpdelay/estimator.h"

struct bpf_object;
struct ring_buffer;

struct tcpdelay_capture {
    struct bpf_object *object;
    struct ring_buffer *ring;
    int socket_descriptor;
    int counters_descriptor;
    /* The interface the socket is bound to; a recreated one needs a new socket. */
    unsigned int interface_index;
    /* One value per possible CPU, for reading the per-CPU counters. */
    uint64_t *counter_values;
    size_t cpu_count;
    /* Borrowed; every drained record is added to it. */
    struct tcpdelay_estimator *estimator;
};

/*
 * Loads the socket filter from object_path and attaches it to a packet socket
 * on interface. Records are only collected by tcpdelay_capture_drain().
 */
int tcpdelay_capture_open(
    struct tcpdelay_capture *capture,
    const char *object_path,
    const char *interface,
    struct tcpdelay_estimator *estimator,
    char *error,
    size_t error_size
);

/* Adds every pending record to the estimator; returns the count or -1. */
int tcpdelay_capture_drain(struct tcpdelay_capture *capture);

/* Totals of the filter's counters, indexed by TCPDELAY_COUNTER_*. */
int tcpdelay_capture_counters(
    const struct tcpdelay_capture *capture,
    uint64_t counters[TCPDELAY_COUNTERS]
);

/* Safe after any tcpdelay_capture_open(), successful or not, and repeatable. */
void tcpdelay_capture_close(struct tcpdelay_capture *capture);

#endif
