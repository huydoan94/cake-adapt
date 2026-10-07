#ifndef TCPDELAY_ESTIMATOR_H_INCLUDED
#define TCPDELAY_ESTIMATOR_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tcpdelay/record.h"

/* Flow slots; the flow with the least recently accepted sample is replaced. */
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

/*
 * The lowest raw delay a flow has shown, taken as its empty path. It moves up
 * only as far as the estimator's queue bound requires.
 */
struct tcpdelay_floor {
	int64_t value;
	bool valid;
};

/* Per-direction minimum queueing delay in the current and previous windows. */
struct tcpdelay_window {
	int64_t minimum_ns[2];
	uint64_t index;
	bool valid[2];
};

struct tcpdelay_flow {
	struct tcpdelay_record_flow key;
	bool used;
	/* Arrival of the last accepted sample; rejected records do not affect LRU. */
	uint64_t last_accepted_ns;
	uint64_t first_arrival_ns;
	uint32_t last_tsval;
	/* Remote timestamp ticks since the first sample, unwrapped. */
	uint64_t ticks;
	/* Remote timestamp clock period, once it snaps to a standard rate. */
	uint64_t tick_ns;
	size_t tick_index;
	/*
	 * Floors for every standard period from the first sample, even though the
	 * period is known seconds later. A flow starting during congestion has an
	 * unknown initial queue, so its zero cannot erase another flow's history.
	 */
	struct tcpdelay_floor download_floor[TCPDELAY_TICKS];
	struct tcpdelay_floor upload_floor[TCPDELAY_TICKS];
	struct tcpdelay_window download;
	struct tcpdelay_window upload;
};

struct tcpdelay_estimator {
	struct tcpdelay_flow flows[TCPDELAY_FLOWS];
	/*
	 * No sample may show a queue above queue_bound_ns: one that would moves its
	 * floor up instead. Without a bound, floors only move down.
	 */
	bool bounded;
	int64_t queue_bound_ns;
};

struct tcpdelay_estimate {
	bool download_valid;
	bool upload_valid;
	int64_t download_queue_us;
	int64_t upload_queue_us;
};

/*
 * The largest queue any flow may show from now on, such as fping's round-trip
 * delay above its baseline: a queue on the access link delays both. A zero
 * bound re-zeroes every floor, absorbing remote clock drift and route changes.
 * Kept in nanoseconds, the filter's clock.
 */
void tcpdelay_estimator_set_bound(struct tcpdelay_estimator *estimator, int64_t queue_bound_us);

void tcpdelay_estimator_add(
	struct tcpdelay_estimator *estimator,
	const struct tcpdelay_sample *sample
);

/* Fresh queues from one flow, preferring a complete pair and the longest history. */
void tcpdelay_estimator_result(
	const struct tcpdelay_estimator *estimator,
	uint64_t now_us,
	struct tcpdelay_estimate *estimate
);

#endif
