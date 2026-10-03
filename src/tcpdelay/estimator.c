#include "tcpdelay/estimator.h"
#include "config/defaults.h"

#include <string.h>

/* Samples carry kernel nanoseconds; the defaults are in microseconds. */
#define WINDOW_NS (TCPDELAY_WINDOW_MICROSECONDS * NANOSECONDS_PER_MICROSECOND)
#define FLOOR_BUCKET_NS (TCPDELAY_FLOOR_BUCKET_MICROSECONDS * NANOSECONDS_PER_MICROSECOND)
#define TICK_SPAN_NS (TCPDELAY_TICK_FIT_MICROSECONDS * NANOSECONDS_PER_MICROSECOND)
#define IMPLAUSIBLE_QUEUE_NS \
    ((int64_t)(TCPDELAY_IMPLAUSIBLE_QUEUE_MICROSECONDS * NANOSECONDS_PER_MICROSECOND))

/* Standard TCP timestamp clock periods (1 ms on Linux and the BSDs). */
static const uint64_t standard_ticks_ns[TCPDELAY_TICKS] = {
    UINT64_C(1000000),
    UINT64_C(4000000),
    UINT64_C(10000000),
    UINT64_C(100000000)
};

static int64_t floor_update(
    struct tcpdelay_floor *floor,
    int64_t value,
    uint64_t now_ns
)
{
    uint64_t bucket = now_ns / FLOOR_BUCKET_NS;

    if (!floor->valid) {
        floor->current = value;
        floor->previous = value;
        floor->bucket = bucket;
        floor->valid = true;
    } else if (bucket != floor->bucket) {
        floor->previous = bucket == floor->bucket + 1U ? floor->current : value;
        floor->current = value;
        floor->bucket = bucket;
    } else if (value < floor->current) {
        floor->current = value;
    }
    return floor->current < floor->previous ? floor->current : floor->previous;
}

static void window_add(
    struct tcpdelay_window *window,
    int64_t queue_ns,
    uint64_t time_ns
)
{
    uint64_t index = time_ns / WINDOW_NS;

    if (!window->valid[0] && !window->valid[1]) {
        window->index = index;
    }
    if (index > window->index) {
        /* The previous window survives only if it is the one just before. */
        window->minimum_ns[1] = window->minimum_ns[0];
        window->valid[1] = window->valid[0] && index == window->index + 1U;
        window->valid[0] = false;
        window->index = index;
    }
    if (index == window->index) {
        if (!window->valid[0] || queue_ns < window->minimum_ns[0]) {
            window->minimum_ns[0] = queue_ns;
            window->valid[0] = true;
        }
    } else if (index + 1U == window->index) {
        if (!window->valid[1] || queue_ns < window->minimum_ns[1]) {
            window->minimum_ns[1] = queue_ns;
            window->valid[1] = true;
        }
    }
}

static bool window_result(
    const struct tcpdelay_window *window,
    uint64_t now_ns,
    int64_t *queue_ns
)
{
    uint64_t index = now_ns / WINDOW_NS;
    bool valid = false;

    /* Only windows that are the current or the previous one at now_ns count. */
    if (window->valid[0] && (window->index == index || window->index + 1U == index)) {
        *queue_ns = window->minimum_ns[0];
        valid = true;
    }
    if (
        window->valid[1] && window->index == index &&
        (!valid || window->minimum_ns[1] < *queue_ns)
    ) {
        *queue_ns = window->minimum_ns[1];
        valid = true;
    }
    return valid;
}

static void flow_start(
    struct tcpdelay_flow *flow,
    const struct tcpdelay_sample *sample
)
{
    memset(flow, 0, sizeof(*flow));
    flow->key = sample->flow;
    flow->used = true;
    flow->first_arrival_ns = sample->arrival_ns;
    flow->last_tsval = sample->tsval;
    flow->last_seen_ns = sample->arrival_ns;
}

static struct tcpdelay_flow *flow_for(
    struct tcpdelay_estimator *estimator,
    const struct tcpdelay_sample *sample
)
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
        if (memcmp(&flow->key, &sample->flow, sizeof(flow->key)) == 0) {
            return flow;
        }
        if (oldest == NULL || flow->last_seen_ns < oldest->last_seen_ns) {
            oldest = flow;
        }
    }
    flow_start(oldest, sample);
    return oldest;
}

/* Adopts the remote clock period once it fits a standard one within 5%. */
static void fit_tick(struct tcpdelay_flow *flow, uint64_t span_ns)
{
    uint64_t period_ns;
    size_t index;

    if (span_ns < TICK_SPAN_NS || flow->ticks == 0U) {
        return;
    }
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

/*
 * Updates the floors for one candidate clock period and returns the queueing
 * delay in each direction; upload_ns is negative when the sample has no
 * departure.
 */
static void measure(
    struct tcpdelay_flow *flow,
    size_t tick_index,
    const struct tcpdelay_sample *sample,
    int64_t *download_ns,
    int64_t *upload_ns
)
{
    int64_t elapsed_ns = (int64_t)(sample->arrival_ns - flow->first_arrival_ns);
    int64_t remote_ns = (int64_t)(flow->ticks * standard_ticks_ns[tick_index]);

    /* Arrival minus remote send time: only the downstream delay varies. */
    *download_ns = elapsed_ns - remote_ns;
    *download_ns -= floor_update(&flow->download_floor[tick_index], *download_ns, sample->arrival_ns);

    /*
     * Remote receive time minus our departure of the echoed TSval: only the
     * upstream delay varies. Delayed ACKs only add, which the window minimum
     * removes.
     */
    *upload_ns = -1;
    if (sample->departure_ns != 0U) {
        int64_t departed_ns = (int64_t)sample->departure_ns - (int64_t)flow->first_arrival_ns;

        *upload_ns = remote_ns - departed_ns;
        *upload_ns -= floor_update(&flow->upload_floor[tick_index], *upload_ns, sample->arrival_ns);
    }
}

void tcpdelay_estimator_init(struct tcpdelay_estimator *estimator)
{
    memset(estimator, 0, sizeof(*estimator));
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
    int64_t download_ns;
    int64_t upload_ns;
    size_t index;

    flow->last_seen_ns = sample->arrival_ns;
    if (step < 0 || sample->arrival_ns < flow->first_arrival_ns) {
        return;
    }
    flow->ticks += (uint64_t)step;
    flow->last_tsval = sample->tsval;
    if (flow->tick_ns == 0U) {
        for (index = 0U; index < TCPDELAY_TICKS; index++) {
            measure(flow, index, sample, &download_ns, &upload_ns);
        }
        fit_tick(flow, sample->arrival_ns - flow->first_arrival_ns);
        return;
    }
    measure(flow, flow->tick_index, sample, &download_ns, &upload_ns);
    if (download_ns > IMPLAUSIBLE_QUEUE_NS || upload_ns > IMPLAUSIBLE_QUEUE_NS) {
        flow_start(flow, sample);
        return;
    }
    window_add(&estimator->download, download_ns, sample->arrival_ns);
    if (upload_ns >= 0) {
        window_add(&estimator->upload, upload_ns, sample->arrival_ns);
    }
}

void tcpdelay_estimator_result(
    const struct tcpdelay_estimator *estimator,
    uint64_t now_ns,
    struct tcpdelay_estimate *estimate
)
{
    int64_t queue_ns = 0;

    memset(estimate, 0, sizeof(*estimate));
    if (window_result(&estimator->download, now_ns, &queue_ns)) {
        estimate->download_valid = true;
        estimate->download_queue_microseconds = queue_ns / (int64_t)NANOSECONDS_PER_MICROSECOND;
    }
    if (window_result(&estimator->upload, now_ns, &queue_ns)) {
        estimate->upload_valid = true;
        estimate->upload_queue_microseconds = queue_ns / (int64_t)NANOSECONDS_PER_MICROSECOND;
    }
}
