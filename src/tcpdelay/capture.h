#ifndef TCPDELAY_CAPTURE_H_INCLUDED
#define TCPDELAY_CAPTURE_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "tcpdelay/estimator.h"
#include "tcpdelay/lifetime.h"

struct bpf_object;
struct ring_buffer;

enum tcpdelay_capture_state {
	TCPDELAY_CAPTURE_READY,
	TCPDELAY_CAPTURE_RECOVERING,
	TCPDELAY_CAPTURE_DISABLED,
};

struct tcpdelay_capture {
	struct bpf_object *object;
	struct ring_buffer *ring;
	int socket_descriptor;
	int counters_descriptor;
	int accounting_descriptor;
	int epoch_descriptor;
	int loss_descriptor;
	int fault_descriptor;
	/* Immutable after bind; changing the model needs a new capture. */
	struct tcpdelay_accounting accounting;
	/* The interface the socket is bound to; a recreated one needs a new socket. */
	unsigned int interface_index;
	/* One copy per possible CPU, for reading the per-CPU counters. */
	struct tcpdelay_counters *counter_values;
	size_t cpu_count;
	uint8_t *fault_values;
	struct tcpdelay_lifetime *lifetime;
	uint32_t epoch;
	enum tcpdelay_capture_state state;
	uint64_t retry_after_microseconds;
	bool uncertain_pending;
	bool ring_busy;
	/* Borrowed; every drained record is added to it. */
	struct tcpdelay_estimator *estimator;
};

/*
 * Loads the socket filter from object_path and attaches it to a packet socket
 * on interface. Records are only collected by tcpdelay_capture_drain().
 * NULL accounting keeps raw counters; a model enables verified CAKE charges.
 */
int tcpdelay_capture_open(
	struct tcpdelay_capture *capture,
	const char *object_path,
	const char *interface,
	const struct cake_accounting *accounting,
	struct tcpdelay_estimator *estimator,
	char *error,
	size_t error_size
);

/* Adds every pending record to the estimator; returns the count or -1. */
int tcpdelay_capture_drain(struct tcpdelay_capture *capture);

bool tcpdelay_capture_timing_available(const struct tcpdelay_capture *capture);

/* The filter's counters summed over all CPUs, in one map lookup. */
int tcpdelay_capture_counters(
	const struct tcpdelay_capture *capture,
	struct tcpdelay_counters *counters
);

/* Safe after any tcpdelay_capture_open(), successful or not, and repeatable. */
void tcpdelay_capture_close(struct tcpdelay_capture *capture);

#endif
