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
	/* One copy per possible CPU, for reading the per-CPU counters. */
	struct tcpdelay_counters *counter_values;
	size_t cpu_count;
	/* Borrowed; every drained record is added to it. */
	struct tcpdelay_estimator *estimator;
};

/*
 * Loads the socket filter from object_path and attaches it to a packet socket
 * on interface. Records are only collected by tcpdelay_capture_drain().
 */
int tcpdelay_capture_open(struct tcpdelay_capture *capture, const char *object_path,
			  const char *interface, struct tcpdelay_estimator *estimator, char *error,
			  size_t error_size);

/* Adds every pending record to the estimator; returns the count or -1. */
int tcpdelay_capture_drain(struct tcpdelay_capture *capture);

/* The filter's counters summed over all CPUs, in one map lookup. */
int tcpdelay_capture_counters(const struct tcpdelay_capture *capture,
			      struct tcpdelay_counters *counters);

/* Safe after any tcpdelay_capture_open(), successful or not, and repeatable. */
void tcpdelay_capture_close(struct tcpdelay_capture *capture);

#endif
