#include "tcpdelay/estimator.h"
#include "common/constants.h"
#include "common/utils.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Times here are nanoseconds, the filter's clock; the estimator's interface takes microseconds. */
/* Fixed one-way path delays outside any queue. */
#define PATH_NS (5U * NANOSECONDS_PER_MILLISECOND)
/* Arbitrary monotonic origin, so departures before the first sample stay positive. */
#define ORIGIN_NS (1000U * NANOSECONDS_PER_MILLISECOND)

struct remote {
	struct tcpdelay_record_flow key;
	uint64_t tick_ns;
	uint32_t tsval_base;
	bool departures;
};

static struct remote remote_flow(uint16_t port, uint64_t tick_ns, uint32_t tsval_base)
{
	struct remote remote = { .tick_ns = tick_ns, .tsval_base = tsval_base, .departures = true };

	remote.key.local_address[15] = 2U;
	remote.key.remote_address[15] = 1U;
	remote.key.local_port = port;
	remote.key.remote_port = 443U;
	return remote;
}

/*
 * The remote sends at sent_ns (its clock running in step with ours) and
 * echoes our segment that departed early enough to cross the upload queue
 * and wait ack_delay_ns at the receiver.
 */
static void send_sample(
	struct tcpdelay_estimator *estimator,
	const struct remote *remote,
	uint64_t sent_ns,
	uint64_t download_queue_ns,
	uint64_t upload_queue_ns,
	uint64_t ack_delay_ns
)
{
	struct tcpdelay_sample sample = {
		.flow = remote->key,
		.arrival_ns = ORIGIN_NS + sent_ns + PATH_NS + download_queue_ns,
		.tsval = remote->tsval_base + (uint32_t)(sent_ns / remote->tick_ns),
	};

	if (remote->departures) {
		sample.departure_ns =
			ORIGIN_NS + sent_ns - ack_delay_ns - upload_queue_ns - PATH_NS;
	}
	tcpdelay_estimator_add(estimator, &sample);
}

/* One packet per millisecond from start_ns until end_ns. */
static uint64_t send_span(
	struct tcpdelay_estimator *estimator,
	const struct remote *remote,
	uint64_t start_ns,
	uint64_t end_ns,
	uint64_t download_queue_ns,
	uint64_t upload_queue_ns
)
{
	uint64_t sent_ns;

	for (sent_ns = start_ns; sent_ns < end_ns; sent_ns += NANOSECONDS_PER_MILLISECOND)
		send_sample(estimator, remote, sent_ns, download_queue_ns, upload_queue_ns, 0U);
	return end_ns;
}

static int64_t nearest_ms(int64_t value_us)
{
	return signed_rounded_divide(value_us, (int64_t)MICROSECONDS_PER_MILLISECOND);
}

/* Result at the arrival time of the last packet sent before sent_ns. */
static void result_after(
	const struct tcpdelay_estimator *estimator,
	uint64_t sent_ns,
	struct tcpdelay_estimate *estimate
)
{
	tcpdelay_estimator_result(
		estimator,
		(ORIGIN_NS + sent_ns + PATH_NS) / NANOSECONDS_PER_MICROSECOND,
		estimate
	);
}

static void assert_close(int64_t value_us, int64_t expected_ms)
{
	int64_t difference = nearest_ms(value_us) - expected_ms;

	if (difference < -2 || difference > 2) {
		fprintf(stderr,
			"queue %lld us, expected %lld ms\n",
			(long long)value_us,
			(long long)expected_ms);
	}
	assert(difference >= -2 && difference <= 2);
}

static void test_no_estimate_before_the_tick_is_known(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50000U, NANOSECONDS_PER_MILLISECOND, 12345U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 1900U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert(!estimate.download_valid);
	assert(!estimate.upload_valid);

	now = send_span(&estimator, &remote, now, 2100U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(estimate.upload_valid);
	assert(estimator.flows[0].tick_ns == NANOSECONDS_PER_MILLISECOND);
}

static void test_queue_attributed_to_its_direction(uint64_t tick_ns, uint32_t tsval_base)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50001U, tick_ns, tsval_base);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	assert(estimator.flows[0].tick_ns == tick_ns);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 0);

	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		40U * NANOSECONDS_PER_MILLISECOND,
		0U
	);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_us, 40);
	assert_close(estimate.upload_queue_us, 0);

	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		0U,
		80U * NANOSECONDS_PER_MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 80);

	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		30U * NANOSECONDS_PER_MILLISECOND,
		120U * NANOSECONDS_PER_MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_us, 30);
	assert_close(estimate.upload_queue_us, 120);
}

/* A queue that fills before the clock period is known still counts in full. */
static void test_queue_built_before_the_tick_is_known(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50007U, NANOSECONDS_PER_MILLISECOND, 500U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 100U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		3000U * NANOSECONDS_PER_MILLISECOND,
		20U * NANOSECONDS_PER_MILLISECOND,
		150U * NANOSECONDS_PER_MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 20);
	assert_close(estimate.upload_queue_us, 150);
}

/* Delayed ACKs only lengthen the upstream estimate; the window minimum removes them. */
static void test_delayed_acks_are_ignored(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50002U, NANOSECONDS_PER_MILLISECOND, 7U);
	uint64_t sent_ns;
	unsigned int packet = 0U;

	memset(&estimator, 0, sizeof(estimator));
	for (sent_ns = 0U; sent_ns < 4000U * NANOSECONDS_PER_MILLISECOND;
	     sent_ns += NANOSECONDS_PER_MILLISECOND) {
		uint64_t ack_delay_ns = packet++ % 5U == 0U ? 0U :
							      40U * NANOSECONDS_PER_MILLISECOND;

		send_sample(
			&estimator,
			&remote,
			sent_ns,
			0U,
			sent_ns >= 3000U * NANOSECONDS_PER_MILLISECOND ?
				60U * NANOSECONDS_PER_MILLISECOND :
				0U,
			ack_delay_ns
		);
	}
	result_after(&estimator, sent_ns, &estimate);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 60);
}

/* A changed receiver wait looks like upload queueing while no prompt echoes remain. */
static void test_sustained_ack_wait_change_and_recovery(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50015U, NANOSECONDS_PER_MILLISECOND, 91U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	for (; now < 4000U * NANOSECONDS_PER_MILLISECOND; now += NANOSECONDS_PER_MILLISECOND)
		send_sample(&estimator, &remote, now, 0U, 0U, 40U * NANOSECONDS_PER_MILLISECOND);
	tcpdelay_estimator_result(
		&estimator,
		(ORIGIN_NS + now - NANOSECONDS_PER_MILLISECOND + PATH_NS) /
			NANOSECONDS_PER_MICROSECOND,
		&estimate
	);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 40);

	/* Confirm recovery after 300 ms; a prompt minimum may restore zero earlier. */
	now = send_span(&estimator, &remote, now, now + 300U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	tcpdelay_estimator_result(
		&estimator,
		(ORIGIN_NS + now - NANOSECONDS_PER_MILLISECOND + PATH_NS) /
			NANOSECONDS_PER_MICROSECOND,
		&estimate
	);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 0);
}

/* A constant receiver wait present during calibration is absorbed by its floor. */
static void test_constant_ack_wait_is_baseline(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50016U, NANOSECONDS_PER_MILLISECOND, 101U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	for (now = 0U; now < 4000U * NANOSECONDS_PER_MILLISECOND;
	     now += NANOSECONDS_PER_MILLISECOND)
		send_sample(&estimator, &remote, now, 0U, 0U, 40U * NANOSECONDS_PER_MILLISECOND);
	tcpdelay_estimator_result(
		&estimator,
		(ORIGIN_NS + now - NANOSECONDS_PER_MILLISECOND + PATH_NS) /
			NANOSECONDS_PER_MICROSECOND,
		&estimate
	);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 0);
}

static void test_results_expire(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50003U, NANOSECONDS_PER_MILLISECOND, 0U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	result_after(&estimator, now + 150U * NANOSECONDS_PER_MILLISECOND, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	result_after(&estimator, now + 250U * NANOSECONDS_PER_MILLISECOND, &estimate);
	assert(!estimate.download_valid);
	assert(!estimate.upload_valid);
}

/* The download and upload queue at the result after the last packet sent before now. */
static void assert_queues(
	const struct tcpdelay_estimator *estimator,
	uint64_t now,
	uint64_t download_queue_ns,
	int64_t download_ms,
	int64_t upload_ms
)
{
	struct tcpdelay_estimate estimate;

	tcpdelay_estimator_result(
		estimator,
		(ORIGIN_NS + now - NANOSECONDS_PER_MILLISECOND + PATH_NS + download_queue_ns) /
			NANOSECONDS_PER_MICROSECOND,
		&estimate
	);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, download_ms);
	assert_close(estimate.upload_queue_us, upload_ms);
}

/*
 * Without a bound a standing queue stays visible however long it lasts; a bound
 * caps it, and a zero bound re-zeroes the floors while later congestion still shows.
 */
static void test_standing_queue_and_the_queue_bound(void)
{
	struct tcpdelay_estimator estimator;
	struct remote remote = remote_flow(50014U, NANOSECONDS_PER_MILLISECOND, 77U);
	const uint64_t download_queue = 80U * NANOSECONDS_PER_MILLISECOND;
	const uint64_t upload_queue = 20U * NANOSECONDS_PER_MILLISECOND;
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		120000U * NANOSECONDS_PER_MILLISECOND,
		download_queue,
		upload_queue
	);
	assert_queues(&estimator, now, download_queue, 80, 20);

	/* fping sees 50 ms added: neither direction may show more. */
	tcpdelay_estimator_set_bound(&estimator, 50 * (int64_t)MILLISECOND);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		download_queue,
		upload_queue
	);
	assert_queues(&estimator, now, download_queue, 50, 20);

	/* A clear path re-zeroes the floors at the current delay. */
	tcpdelay_estimator_set_bound(&estimator, 0);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		download_queue,
		upload_queue
	);
	assert_queues(&estimator, now, download_queue, 0, 0);

	/* Congestion on top of the new floors shows up to the bound fping reports. */
	tcpdelay_estimator_set_bound(&estimator, 100 * (int64_t)MILLISECOND);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		download_queue + 40U * NANOSECONDS_PER_MILLISECOND,
		upload_queue + 15U * NANOSECONDS_PER_MILLISECOND
	);
	assert_queues(&estimator, now, download_queue + 40U * NANOSECONDS_PER_MILLISECOND, 40, 15);
}

static void test_hold_preserves_standing_queue(uint64_t phase_ns)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50017U, NANOSECONDS_PER_MILLISECOND, 113U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(
		&estimator,
		&remote,
		phase_ns,
		phase_ns + 3000U * NANOSECONDS_PER_MILLISECOND,
		0U,
		0U
	);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 61000U * NANOSECONDS_PER_MILLISECOND,
		80U * NANOSECONDS_PER_MILLISECOND,
		20U * NANOSECONDS_PER_MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 80);
	assert_close(estimate.upload_queue_us, 20);

	/* The original low-delay floor keeps the same standing queue visible. */
	now = send_span(&estimator, &remote, now, now + 1000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		80U * NANOSECONDS_PER_MILLISECOND,
		20U * NANOSECONDS_PER_MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_us, 80);
	assert_close(estimate.upload_queue_us, 20);
}

static void test_floor_accepts_lower_raw_minimum(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50019U, NANOSECONDS_PER_MILLISECOND, 131U);
	struct tcpdelay_floor *download_floor;
	struct tcpdelay_floor *upload_floor;
	int64_t initial_download_floor;
	int64_t initial_upload_floor;
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(
		&estimator,
		&remote,
		0U,
		3000U * NANOSECONDS_PER_MILLISECOND,
		80U * NANOSECONDS_PER_MILLISECOND,
		20U * NANOSECONDS_PER_MILLISECOND
	);
	initial_download_floor = estimator.flows[0].download_floor[0].value_ns;
	initial_upload_floor = estimator.flows[0].upload_floor[0].value_ns;
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		60U * NANOSECONDS_PER_MILLISECOND,
		10U * NANOSECONDS_PER_MILLISECOND
	);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		40U * NANOSECONDS_PER_MILLISECOND,
		5U * NANOSECONDS_PER_MILLISECOND
	);

	download_floor = &estimator.flows[0].download_floor[0];
	upload_floor = &estimator.flows[0].upload_floor[0];
	assert(download_floor->value_ns < initial_download_floor);
	assert(upload_floor->value_ns < initial_upload_floor);

	/* Returning to the initial 80/20-ms path shows delay relative to the lower floor. */
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		80U * NANOSECONDS_PER_MILLISECOND,
		20U * NANOSECONDS_PER_MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 40);
	assert_close(estimate.upload_queue_us, 15);
}

/* The floor follows new minimums down and rises only by what exceeds the bound. */
static void test_floor_follows_the_bound(void)
{
	struct tcpdelay_estimator estimator;
	struct remote remote = remote_flow(50018U, NANOSECONDS_PER_MILLISECOND, 127U);
	const struct tcpdelay_floor *floor = &estimator.flows[0].download_floor[0];
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(
		&estimator,
		&remote,
		0U,
		3000U * NANOSECONDS_PER_MILLISECOND,
		80U * NANOSECONDS_PER_MILLISECOND,
		0U
	);
	assert(floor->valid && floor->value_ns == 0);

	/* Unbounded, a higher delay leaves the floor where it is. */
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		140U * NANOSECONDS_PER_MILLISECOND,
		0U
	);
	assert(floor->value_ns == 0);

	/* A 20 ms bound lifts it to 20 ms below the delay. */
	tcpdelay_estimator_set_bound(&estimator, 20 * (int64_t)MILLISECOND);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		140U * NANOSECONDS_PER_MILLISECOND,
		0U
	);
	assert(floor->value_ns == (int64_t)(40U * NANOSECONDS_PER_MILLISECOND));

	/* A lower delay is a new minimum, whatever the bound. */
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * NANOSECONDS_PER_MILLISECOND,
		100U * NANOSECONDS_PER_MILLISECOND,
		0U
	);
	assert(floor->value_ns == (int64_t)(20U * NANOSECONDS_PER_MILLISECOND));
}

static void test_samples_without_departure(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50004U, NANOSECONDS_PER_MILLISECOND, 99U);
	uint64_t now;

	remote.departures = false;
	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 500U * NANOSECONDS_PER_MILLISECOND,
		25U * NANOSECONDS_PER_MILLISECOND,
		0U
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(!estimate.upload_valid);
	assert_close(estimate.download_queue_us, 25);
}

/* A new flow's congested baseline cannot erase an established flow's queue. */
static void test_new_flow_keeps_established_queues(bool departures)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote established = remote_flow(50008U, NANOSECONDS_PER_MILLISECOND, 1000U);
	struct remote newcomer = remote_flow(50009U, NANOSECONDS_PER_MILLISECOND, 2000U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &established, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&established,
		now,
		4000U * NANOSECONDS_PER_MILLISECOND,
		80U * NANOSECONDS_PER_MILLISECOND,
		20U * NANOSECONDS_PER_MILLISECOND
	);
	newcomer.departures = departures;
	for (; now < 7000U * NANOSECONDS_PER_MILLISECOND; now += NANOSECONDS_PER_MILLISECOND) {
		send_sample(
			&estimator,
			&established,
			now,
			80U * NANOSECONDS_PER_MILLISECOND,
			20U * NANOSECONDS_PER_MILLISECOND,
			0U
		);
		send_sample(
			&estimator,
			&newcomer,
			now,
			80U * NANOSECONDS_PER_MILLISECOND,
			20U * NANOSECONDS_PER_MILLISECOND,
			0U
		);
	}
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 80);
	assert_close(estimate.upload_queue_us, 20);

	/* Once A stops, B may supply its own pair, but never A's stale upload. */
	now = send_span(
		&estimator,
		&newcomer,
		now,
		7300U * NANOSECONDS_PER_MILLISECOND,
		80U * NANOSECONDS_PER_MILLISECOND,
		20U * NANOSECONDS_PER_MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(estimate.upload_valid == departures);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 0);
}

/* Never synthesize a pair from an older download-only flow and a newer upload. */
static void test_directional_pair_comes_from_one_flow(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote download_only = remote_flow(50010U, NANOSECONDS_PER_MILLISECOND, 1000U);
	struct remote paired = remote_flow(50011U, NANOSECONDS_PER_MILLISECOND, 2000U);
	uint64_t now;

	download_only.departures = false;
	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &download_only, 0U, 1000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	for (; now < 4000U * NANOSECONDS_PER_MILLISECOND; now += NANOSECONDS_PER_MILLISECOND) {
		send_sample(&estimator, &download_only, now, 0U, 0U, 0U);
		send_sample(&estimator, &paired, now, 0U, 0U, 0U);
	}
	for (; now < 4500U * NANOSECONDS_PER_MILLISECOND; now += NANOSECONDS_PER_MILLISECOND) {
		send_sample(&estimator, &download_only, now, 0U, 0U, 0U);
		send_sample(
			&estimator,
			&paired,
			now,
			40U * NANOSECONDS_PER_MILLISECOND,
			80U * NANOSECONDS_PER_MILLISECOND,
			0U
		);
	}
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 40);
	assert_close(estimate.upload_queue_us, 80);

	/* The surviving partial flow remains available after the pair expires. */
	now = send_span(&estimator, &download_only, now, 5000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(!estimate.upload_valid);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 0);
}

static void test_reordered_packet_is_skipped(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50005U, NANOSECONDS_PER_MILLISECOND, 0xfffffff0U);
	uint64_t now;
	uint64_t ticks;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	ticks = estimator.flows[0].ticks;
	/* Sent 20 ms ago, so its TSval is older than the last one seen. */
	send_sample(&estimator, &remote, now - 20U * NANOSECONDS_PER_MILLISECOND, 0U, 0U, 0U);
	assert(estimator.flows[0].ticks == ticks);
	now = send_span(&estimator, &remote, now, now + 500U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 0);
}

static void test_later_arrival_with_older_timestamp_is_ignored(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_sample sample;
	struct remote remote = remote_flow(50012U, NANOSECONDS_PER_MILLISECOND, 1000U);
	struct tcpdelay_flow before;

	memset(&estimator, 0, sizeof(estimator));
	send_span(&estimator, &remote, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	memcpy(&before, &estimator.flows[0], sizeof(before));
	sample.flow = before.key;
	/* send_span is end-exclusive, so the last sent time is 2999 ms. */
	sample.arrival_ns = ORIGIN_NS + 3000U * NANOSECONDS_PER_MILLISECOND + PATH_NS;
	sample.departure_ns = 0U;
	sample.tsval = before.last_tsval - 1U;
	tcpdelay_estimator_add(&estimator, &sample);

	assert(memcmp(&estimator.flows[0], &before, sizeof(before)) == 0);
}

static void test_earlier_arrival_with_newer_timestamp_is_ignored(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_sample sample;
	struct remote remote = remote_flow(50013U, NANOSECONDS_PER_MILLISECOND, 1000U);
	struct tcpdelay_flow before;

	memset(&estimator, 0, sizeof(estimator));
	send_span(&estimator, &remote, 0U, 3000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	memcpy(&before, &estimator.flows[0], sizeof(before));
	sample.flow = before.key;
	/* One millisecond before the last packet produced by the end-exclusive span. */
	sample.arrival_ns = ORIGIN_NS + 2998U * NANOSECONDS_PER_MILLISECOND + PATH_NS;
	sample.departure_ns = 0U;
	sample.tsval = before.last_tsval + 1U;
	tcpdelay_estimator_add(&estimator, &sample);

	assert(memcmp(&estimator.flows[0], &before, sizeof(before)) == 0);
}

static void test_equal_timestamp_with_later_arrival_refreshes_lru(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_sample sample = { 0 };
	size_t index;
	bool oldest = false;
	bool second_oldest = false;
	bool newest = false;

	memset(&estimator, 0, sizeof(estimator));
	for (index = 0U; index < TCPDELAY_FLOWS; index++) {
		sample.flow.local_port = (uint16_t)(41000U + index);
		sample.arrival_ns = ORIGIN_NS + index * NANOSECONDS_PER_MILLISECOND;
		sample.tsval = (uint32_t)(100U + index);
		tcpdelay_estimator_add(&estimator, &sample);
	}

	/* Coarse remote clocks repeat TSval; a later arrival still refreshes LRU. */
	sample.flow = estimator.flows[0].key;
	sample.arrival_ns = ORIGIN_NS + TCPDELAY_FLOWS * NANOSECONDS_PER_MILLISECOND;
	sample.tsval = estimator.flows[0].last_tsval;
	tcpdelay_estimator_add(&estimator, &sample);

	sample.flow.local_port = (uint16_t)(41000U + TCPDELAY_FLOWS);
	sample.arrival_ns += NANOSECONDS_PER_MILLISECOND;
	sample.tsval = 500U;
	tcpdelay_estimator_add(&estimator, &sample);
	for (index = 0U; index < TCPDELAY_FLOWS; index++) {
		assert(estimator.flows[index].used);
		oldest |= estimator.flows[index].key.local_port == 41000U;
		second_oldest |= estimator.flows[index].key.local_port == 41001U;
		newest |= estimator.flows[index].key.local_port == 41000U + TCPDELAY_FLOWS;
	}
	assert(oldest);
	assert(!second_oldest);
	assert(newest);
}

static void test_newer_timestamp_with_equal_arrival_is_accepted(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_sample sample = {
		.flow = { .local_port = 42000U },
		.arrival_ns = ORIGIN_NS,
		.tsval = 100U,
	};

	memset(&estimator, 0, sizeof(estimator));
	tcpdelay_estimator_add(&estimator, &sample);
	sample.tsval++;
	tcpdelay_estimator_add(&estimator, &sample);

	assert(estimator.flows[0].ticks == 1U);
	assert(estimator.flows[0].last_tsval == sample.tsval);
}

/* An unknown clock rate is never adopted; that flow gives no estimate. */
static void test_nonstandard_tick_is_rejected(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50006U, 2U * NANOSECONDS_PER_MILLISECOND, 0U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 5000U * NANOSECONDS_PER_MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert(estimator.flows[0].tick_ns == 0U);
	assert(!estimate.download_valid);
}

/* Rejected packets must not refresh LRU state in a full table. */
static void test_rejected_sample_does_not_refresh_lru(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_sample sample;
	size_t index;
	bool newest = false;
	bool oldest = false;
	bool second_oldest = false;

	memset(&estimator, 0, sizeof(estimator));
	memset(&sample, 0, sizeof(sample));
	for (index = 0U; index < TCPDELAY_FLOWS; index++) {
		sample.flow.local_port = (uint16_t)(40000U + index);
		sample.arrival_ns = ORIGIN_NS + index * NANOSECONDS_PER_MILLISECOND;
		sample.tsval = (uint32_t)(100U + index);
		tcpdelay_estimator_add(&estimator, &sample);
	}

	/* This reordered record arrives later but must not keep slot 0 active. */
	sample.flow = estimator.flows[0].key;
	sample.arrival_ns = ORIGIN_NS + TCPDELAY_FLOWS * NANOSECONDS_PER_MILLISECOND;
	sample.tsval = estimator.flows[0].last_tsval - 1U;
	tcpdelay_estimator_add(&estimator, &sample);

	sample.flow.local_port = (uint16_t)(40000U + TCPDELAY_FLOWS);
	sample.arrival_ns += NANOSECONDS_PER_MILLISECOND;
	sample.tsval = 500U;
	tcpdelay_estimator_add(&estimator, &sample);
	for (index = 0U; index < TCPDELAY_FLOWS; index++) {
		assert(estimator.flows[index].used);
		oldest |= estimator.flows[index].key.local_port == 40000U;
		second_oldest |= estimator.flows[index].key.local_port == 40001U;
		newest |= estimator.flows[index].key.local_port == 40000U + TCPDELAY_FLOWS;
	}
	assert(newest);
	assert(!oldest);
	assert(second_oldest);
}

/*
 * A remote clock 2% fast still snaps to the 1 ms period, so the snapped clock
 * drifts from real time by 2% of the flow's age: 12 s in 10 minutes. On an
 * empty path neither direction may report a queue, after a clear fping reply
 * and through a long stretch without one, as in IDLE.
 */
static void test_remote_clock_drift_on_a_clear_path(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote =
		remote_flow(50004U, 980U * NANOSECONDS_PER_MILLISECOND / 1000U, 777U);
	uint64_t sent_ns;

	memset(&estimator, 0, sizeof(estimator));
	tcpdelay_estimator_set_bound(&estimator, 0);
	for (sent_ns = 0U; sent_ns < 600000U * NANOSECONDS_PER_MILLISECOND;
	     sent_ns += 10U * NANOSECONDS_PER_MILLISECOND)
		send_sample(&estimator, &remote, sent_ns, 0U, 0U, 0U);
	result_after(&estimator, sent_ns, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_us, 0);
	assert_close(estimate.upload_queue_us, 0);
}

int main(void)
{
	test_remote_clock_drift_on_a_clear_path();
	test_no_estimate_before_the_tick_is_known();
	test_queue_attributed_to_its_direction(NANOSECONDS_PER_MILLISECOND, 1000U);
	test_queue_attributed_to_its_direction(4U * NANOSECONDS_PER_MILLISECOND, 1000U);
	test_queue_attributed_to_its_direction(10U * NANOSECONDS_PER_MILLISECOND, 1000U);
	/* TSval wraps about 0.2 s into the run. */
	test_queue_attributed_to_its_direction(NANOSECONDS_PER_MILLISECOND, 0xffffff00U);
	test_queue_built_before_the_tick_is_known();
	test_delayed_acks_are_ignored();
	test_sustained_ack_wait_change_and_recovery();
	test_constant_ack_wait_is_baseline();
	test_results_expire();
	test_standing_queue_and_the_queue_bound();
	test_hold_preserves_standing_queue(2000U * NANOSECONDS_PER_MILLISECOND);
	test_hold_preserves_standing_queue(22000U * NANOSECONDS_PER_MILLISECOND);
	test_floor_accepts_lower_raw_minimum();
	test_floor_follows_the_bound();
	test_samples_without_departure();
	test_new_flow_keeps_established_queues(false);
	test_new_flow_keeps_established_queues(true);
	test_directional_pair_comes_from_one_flow();
	test_reordered_packet_is_skipped();
	test_later_arrival_with_older_timestamp_is_ignored();
	test_earlier_arrival_with_newer_timestamp_is_ignored();
	test_equal_timestamp_with_later_arrival_refreshes_lru();
	test_newer_timestamp_with_equal_arrival_is_accepted();
	test_nonstandard_tick_is_rejected();
	test_rejected_sample_does_not_refresh_lru();
	puts("tcpdelay estimator tests passed");
	return 0;
}
