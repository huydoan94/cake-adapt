#include "tcpdelay/estimator.h"
#include "config/defaults.h"
#include "common/utils.h"

#include <string.h>

/* Samples carry kernel nanoseconds; the defaults are in microseconds. */
#define WINDOW_NS (TCPDELAY_WINDOW_US * NANOSECONDS_PER_US)
#define TICK_SPAN_NS (TCPDELAY_TICK_FIT_US * NANOSECONDS_PER_US)
#define IMPLAUSIBLE_QUEUE_NS ((int64_t)(TCPDELAY_IMPLAUSIBLE_QUEUE_US * NANOSECONDS_PER_US))

/* Standard TCP timestamp clock periods (1 ms on Linux and the BSDs). */
static const uint64_t standard_ticks_ns[TCPDELAY_TICKS] = {
	UINT64_C(1000000),
	UINT64_C(4000000),
	UINT64_C(10000000),
	UINT64_C(100000000),
};

/*
 * A new minimum lowers the floor; a value more than the bound above it raises
 * the floor to keep the queue at the bound. Returns the floor.
 */
static int64_t
floor_update(const struct tcpdelay_estimator *estimator, struct tcpdelay_floor *floor, int64_t value)
{
	if (!floor->valid || value < floor->value)
		floor->value = value;
	else if (estimator->bounded && value - floor->value > estimator->queue_bound_ns)
		floor->value = value - estimator->queue_bound_ns;
	floor->valid = true;
	return floor->value;
}

static void window_add(struct tcpdelay_window *window, int64_t queue_ns, uint64_t time_ns)
{
	uint64_t index = time_ns / WINDOW_NS;
	uint64_t slot;

	if (!window->valid[0] && !window->valid[1])
		window->index = index;
	if (index > window->index) {
		/* The previous window survives only if it is the one just before. */
		window->minimum_ns[1] = window->minimum_ns[0];
		window->valid[1] = window->valid[0] && index == window->index + 1U;
		window->valid[0] = false;
		window->index = index;
	}
	/* Slot 0 is the current window and slot 1 the one before; older samples count nowhere. */
	slot = window->index - index;
	if (slot > 1U)
		return;
	if (!window->valid[slot] || queue_ns < window->minimum_ns[slot]) {
		window->minimum_ns[slot] = queue_ns;
		window->valid[slot] = true;
	}
}

static bool window_result(const struct tcpdelay_window *window, uint64_t now_ns, int64_t *queue_ns)
{
	uint64_t index = now_ns / WINDOW_NS;
	bool valid = false;

	/* Only windows that are the current or the previous one at now_ns count. */
	if (window->valid[0] && (window->index == index || window->index + 1U == index)) {
		*queue_ns = window->minimum_ns[0];
		valid = true;
	}
	if (window->valid[1] && window->index == index &&
	    (!valid || window->minimum_ns[1] < *queue_ns)) {
		*queue_ns = window->minimum_ns[1];
		valid = true;
	}
	return valid;
}

static void flow_start(struct tcpdelay_flow *flow, const struct tcpdelay_sample *sample)
{
	memset(flow, 0, sizeof(*flow));
	flow->key = sample->flow;
	flow->used = true;
	flow->first_arrival_ns = sample->arrival_ns;
	flow->last_tsval = sample->tsval;
	flow->last_accepted_ns = sample->arrival_ns;
}

static struct tcpdelay_flow *
flow_for(struct tcpdelay_estimator *estimator, const struct tcpdelay_sample *sample)
{
	struct tcpdelay_flow *oldest = NULL;
	size_t index;

	/* Slots fill in order and are never freed, so the first unused one ends the table. */
	for (index = 0U; index < TCPDELAY_FLOWS; index++) {
		struct tcpdelay_flow *flow = &estimator->flows[index];

		if (!flow->used) {
			oldest = flow;
			break;
		}
		if (memcmp(&flow->key, &sample->flow, sizeof(flow->key)) == 0)
			return flow;
		if (oldest == NULL || flow->last_accepted_ns < oldest->last_accepted_ns)
			oldest = flow;
	}
	flow_start(oldest, sample);
	return oldest;
}

/* Adopts the remote clock period once it fits a standard one within 5%. */
static void fit_tick(struct tcpdelay_flow *flow, uint64_t span_ns)
{
	uint64_t period_ns;
	size_t index;

	if (span_ns < TICK_SPAN_NS || flow->ticks == 0U)
		return;
	period_ns = span_ns / flow->ticks;
	for (index = 0U; index < TCPDELAY_TICKS; index++) {
		uint64_t standard = standard_ticks_ns[index];
		uint64_t error = period_ns > standard ? period_ns - standard : standard - period_ns;

		if (error * 20U <= standard) {
			flow->tick_ns = standard;
			flow->tick_index = index;
			return;
		}
	}
}

/* One sample's queueing delay in each direction; upload_ns is negative without a departure. */
struct flow_queues {
	int64_t download_ns;
	int64_t upload_ns;
};

/* Updates the floors for one candidate clock period and returns the sample's queues. */
static struct flow_queues flow_measure(
	const struct tcpdelay_estimator *estimator,
	struct tcpdelay_flow *flow,
	size_t tick_index,
	const struct tcpdelay_sample *sample
)
{
	int64_t elapsed_ns = (int64_t)(sample->arrival_ns - flow->first_arrival_ns);
	int64_t remote_ns = (int64_t)(flow->ticks * standard_ticks_ns[tick_index]);
	struct flow_queues queues = { .upload_ns = -1 };

	/* Arrival minus remote send time: only the downstream delay varies. */
	queues.download_ns = elapsed_ns - remote_ns;
	queues.download_ns -=
		floor_update(estimator, &flow->download_floor[tick_index], queues.download_ns);

	/*
	 * Remote send timestamp minus our departure of the echoed TSval includes both
	 * upload path delay and receiver response wait. A window minimum can suppress
	 * intermittent waits only while prompt echoes remain in its result windows.
	 */
	if (sample->departure_ns != 0U) {
		int64_t departed_ns =
			(int64_t)sample->departure_ns - (int64_t)flow->first_arrival_ns;

		queues.upload_ns = remote_ns - departed_ns;
		queues.upload_ns -=
			floor_update(estimator, &flow->upload_floor[tick_index], queues.upload_ns);
	}
	return queues;
}

void tcpdelay_estimator_set_bound(struct tcpdelay_estimator *estimator, int64_t queue_bound_us)
{
	estimator->bounded = true;
	estimator->queue_bound_ns = queue_bound_us * (int64_t)NANOSECONDS_PER_US;
}

void tcpdelay_estimator_add(
	struct tcpdelay_estimator *estimator,
	const struct tcpdelay_sample *sample
)
{
	struct tcpdelay_flow *flow = flow_for(estimator, sample);
	/*
	 * TSval wraps at 32 bits, so progress is the difference read as signed
	 * (RFC 7323). A reordered packet carries an older TSval and is skipped.
	 */
	int32_t step = (int32_t)(sample->tsval - flow->last_tsval);
	struct flow_queues queues;
	size_t index;

	if (step < 0 || sample->arrival_ns < flow->last_accepted_ns)
		return;
	flow->last_accepted_ns = sample->arrival_ns;
	flow->ticks += (uint64_t)step;
	flow->last_tsval = sample->tsval;
	if (flow->tick_ns == 0U) {
		for (index = 0U; index < TCPDELAY_TICKS; index++)
			(void)flow_measure(estimator, flow, index, sample);
		fit_tick(flow, sample->arrival_ns - flow->first_arrival_ns);
		return;
	}
	queues = flow_measure(estimator, flow, flow->tick_index, sample);
	if (queues.download_ns > IMPLAUSIBLE_QUEUE_NS || queues.upload_ns > IMPLAUSIBLE_QUEUE_NS) {
		flow_start(flow, sample);
		return;
	}
	window_add(&flow->download, queues.download_ns, sample->arrival_ns);
	if (queues.upload_ns >= 0)
		window_add(&flow->upload, queues.upload_ns, sample->arrival_ns);
}

void tcpdelay_estimator_result(
	const struct tcpdelay_estimator *estimator,
	uint64_t now_us,
	struct tcpdelay_estimate *estimate
)
{
	/* The filter timestamps with CLOCK_MONOTONIC, like the monitor. */
	uint64_t now_ns = now_us * NANOSECONDS_PER_US;
	const struct tcpdelay_flow *selected = NULL;
	size_t index;

	memset(estimate, 0, sizeof(*estimate));
	for (index = 0U; index < TCPDELAY_FLOWS && estimator->flows[index].used; index++) {
		const struct tcpdelay_flow *flow = &estimator->flows[index];
		int64_t download_ns;
		int64_t upload_ns = 0;
		bool upload_valid;

		if (!window_result(&flow->download, now_ns, &download_ns))
			continue;
		upload_valid = window_result(&flow->upload, now_ns, &upload_ns);
		/*
		 * Never combine different flows' floors into one directional pair.
		 * Prefer both directions, then the history least likely to have begun
		 * inside a new queue. Age is not proof of an uncongested baseline.
		 */
		if (selected != NULL) {
			if (estimate->upload_valid && !upload_valid)
				continue;
			if (estimate->upload_valid == upload_valid &&
			    selected->first_arrival_ns <= flow->first_arrival_ns) {
				continue;
			}
		}
		selected = flow;
		estimate->download_valid = true;
		estimate->download_queue_us = signed_nanosec_to_us(download_ns);
		estimate->upload_valid = upload_valid;
		estimate->upload_queue_us = upload_valid ? signed_nanosec_to_us(upload_ns) : 0;
	}
}
