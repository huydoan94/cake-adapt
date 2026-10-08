#ifndef TCPDELAY_INJECTOR_H_INCLUDED
#define TCPDELAY_INJECTOR_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>

#include "tcpdelay/capture.h"
#include "tcpdelay/inject.h"

struct bpf_link;

/*
 * The experimental TCP timestamp injection: the tcx egress program in the TCP
 * filter's object (inject.h has the policy), and its link. The capture loads
 * it with tcpdelay_capture_enable_injection().
 */
struct tcpdelay_injector {
	struct bpf_link *egress;
	int counters_descriptor;
	/* One copy per possible CPU, for reading the per-CPU counters. */
	struct tcpdelay_inject_counters *counter_values;
	size_t cpu_count;
};

/* The detached state, before the first attachment or after unloading. */
void tcpdelay_injector_init(struct tcpdelay_injector *injector);

/*
 * Attaches the capture's injector program to interface, after any earlier
 * attachment is removed. The link belongs to this process, so the kernel
 * detaches the program when it exits, however it exits.
 */
int tcpdelay_injector_attach(
	struct tcpdelay_injector *injector,
	const struct tcpdelay_capture *capture,
	const char *interface,
	char *error,
	size_t error_size
);

bool tcpdelay_injector_attached(const struct tcpdelay_injector *injector);

/* The counters summed over all CPUs, cumulative since the capture loaded. */
int tcpdelay_injector_counters(
	const struct tcpdelay_injector *injector,
	struct tcpdelay_inject_counters *counters
);

/* Detaches the program; its maps and server cache stay with the capture. Repeatable. */
void tcpdelay_injector_detach(struct tcpdelay_injector *injector);

/* Detaches and frees everything; safe in any state and repeatable. */
void tcpdelay_injector_unload(struct tcpdelay_injector *injector);

#endif
