#ifndef TCPDELAY_ESTIMATOR_H_INCLUDED
#define TCPDELAY_ESTIMATOR_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tcpdelay/record.h"

/* Flows tracked at once; the least recently seen one is replaced. */
#define TCPDELAY_FLOWS 32U
/* Standard TCP timestamp clock periods: 1, 4, 10 and 100 ms. */
#define TCPDELAY_TICKS 4U

/* One arriving packet with TCP timestamps, as seen at the WAN interface. */
struct tcpdelay_sample {
	struct tcpdelay_record_flow flow;
	uint64_t arrival_ns;
	/* When the packet whose TSval this one echoes left; zero when unknown. */
	uint64_t departure_ns;
	uint32_t tsval;
};

/* Minimum of a value over two alternating time buckets. */
struct tcpdelay_floor {
	int64_t current;
	int64_t previous;
	uint64_t bucket;
	bool valid;
};

struct tcpdelay_flow {
	struct tcpdelay_record_flow key;
	bool used;
	uint64_t last_seen_ns;
	uint64_t first_arrival_ns;
	uint32_t last_tsval;
	/* Remote timestamp ticks since the first sample, unwrapped. */
	uint64_t ticks;
	/* Remote timestamp clock period, once it snaps to a standard rate. */
	uint64_t tick_ns;
	size_t tick_index;
	/*
	 * Floors for every standard period from the first sample, so the empty
	 * queue at the start of a flow sets the floor even though the period is
	 * only known seconds later.
	 */
	struct tcpdelay_floor download_floor[TCPDELAY_TICKS];
	struct tcpdelay_floor upload_floor[TCPDELAY_TICKS];
};

/* Per-direction minimum queueing delay in the current and previous windows. */
struct tcpdelay_window {
	int64_t minimum_ns[2];
	uint64_t index;
	bool valid[2];
};

struct tcpdelay_estimator {
	struct tcpdelay_flow flows[TCPDELAY_FLOWS];
	struct tcpdelay_window download;
	struct tcpdelay_window upload;
};

struct tcpdelay_estimate {
	bool download_valid;
	bool upload_valid;
	int64_t download_queue_microseconds;
	int64_t upload_queue_microseconds;
};

void tcpdelay_estimator_init(struct tcpdelay_estimator *estimator);

void tcpdelay_estimator_add(
	struct tcpdelay_estimator *estimator,
	const struct tcpdelay_sample *sample
);

/* Queueing delay per direction over the last two windows before now_ns. */
void tcpdelay_estimator_result(
	const struct tcpdelay_estimator *estimator,
	uint64_t now_ns,
	struct tcpdelay_estimate *estimate
);

#endif
