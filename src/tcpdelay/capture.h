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
	int program_descriptor;
	int socket_descriptor;
	int counters_descriptor;
	int accounting_descriptor;
	/* Immutable after bind; changing the model needs a new capture. */
	struct tcpdelay_accounting accounting;
	/* The interface the socket is bound to; a recreated one needs a new socket. */
	unsigned int interface_index;
	/* One copy per possible CPU, for reading the per-CPU counters. */
	struct tcpdelay_counters *counter_values;
	size_t cpu_count;
	/* Every drained record is added to it; it starts empty with each capture. */
	struct tcpdelay_estimator estimator;
};

/* The unloaded state, before tcpdelay_capture_load() or after unloading. */
void tcpdelay_capture_init(struct tcpdelay_capture *capture);

/*
 * Loads the installed socket filter, its maps and ring buffer. The kernel's
 * verifier takes seconds on slow CPUs, so this runs once, before the event
 * loop handles latency; later calls do nothing.
 */
int tcpdelay_capture_load(struct tcpdelay_capture *capture, char *error, size_t error_size);

/*
 * Attaches the loaded filter (loading it first if needed) to a new packet
 * socket on interface, with zeroed counters and an empty estimator. Records
 * are only collected by tcpdelay_capture_drain(). NULL accounting keeps raw
 * counters; a model enables verified CAKE charges.
 */
int tcpdelay_capture_open(
	struct tcpdelay_capture *capture,
	const char *interface,
	const struct cake_accounting *accounting,
	char *error,
	size_t error_size
);

/* Adds every pending record to the estimator; returns the count or -1. */
int tcpdelay_capture_drain(struct tcpdelay_capture *capture);

/* The filter's counters summed over all CPUs, in one map lookup. */
int tcpdelay_capture_counters(
	const struct tcpdelay_capture *capture,
	struct tcpdelay_counters *counters
);

/* Detaches the filter by closing the socket; the program stays loaded. Repeatable. */
void tcpdelay_capture_close(struct tcpdelay_capture *capture);

/* Closes and frees everything; safe in any state and repeatable. */
void tcpdelay_capture_unload(struct tcpdelay_capture *capture);

#endif
