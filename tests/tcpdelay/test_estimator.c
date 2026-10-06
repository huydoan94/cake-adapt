#include "tcpdelay/estimator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MILLISECOND UINT64_C(1000000)
/* Fixed one-way path delays outside any queue. */
#define PATH_NS (5U * MILLISECOND)
/* Arbitrary monotonic origin, so departures before the first sample stay positive. */
#define ORIGIN_NS (1000U * MILLISECOND)

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

	for (sent_ns = start_ns; sent_ns < end_ns; sent_ns += MILLISECOND)
		send_sample(estimator, remote, sent_ns, download_queue_ns, upload_queue_ns, 0U);
	return end_ns;
}

static int64_t milliseconds(int64_t microseconds)
{
	return (microseconds + 500) / 1000;
}

/* Result at the arrival time of the last packet sent before sent_ns. */
static void result_after(
	const struct tcpdelay_estimator *estimator,
	uint64_t sent_ns,
	struct tcpdelay_estimate *estimate
)
{
	tcpdelay_estimator_result(estimator, ORIGIN_NS + sent_ns + PATH_NS, estimate);
}

static void assert_close(int64_t microseconds, int64_t expected_milliseconds)
{
	int64_t difference = milliseconds(microseconds) - expected_milliseconds;

	if (difference < -2 || difference > 2) {
		fprintf(stderr,
			"queue %lld us, expected %lld ms\n",
			(long long)microseconds,
			(long long)expected_milliseconds);
	}
	assert(difference >= -2 && difference <= 2);
}

static void test_no_estimate_before_the_tick_is_known(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50000U, MILLISECOND, 12345U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 1900U * MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert(!estimate.download_valid);
	assert(!estimate.upload_valid);

	now = send_span(&estimator, &remote, now, 2100U * MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(estimate.upload_valid);
	assert(estimator.flows[0].tick_ns == MILLISECOND);
}

static void test_queue_attributed_to_its_direction(uint64_t tick_ns, uint32_t tsval_base)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50001U, tick_ns, tsval_base);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	assert(estimator.flows[0].tick_ns == tick_ns);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 0);

	now = send_span(&estimator, &remote, now, now + 1000U * MILLISECOND, 40U * MILLISECOND, 0U);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_microseconds, 40);
	assert_close(estimate.upload_queue_microseconds, 0);

	now = send_span(&estimator, &remote, now, now + 1000U * MILLISECOND, 0U, 80U * MILLISECOND);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 80);

	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * MILLISECOND,
		30U * MILLISECOND,
		120U * MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_microseconds, 30);
	assert_close(estimate.upload_queue_microseconds, 120);
}

/* A queue that fills before the clock period is known still counts in full. */
static void test_queue_built_before_the_tick_is_known(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50007U, MILLISECOND, 500U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 100U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		3000U * MILLISECOND,
		20U * MILLISECOND,
		150U * MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 20);
	assert_close(estimate.upload_queue_microseconds, 150);
}

/* Delayed ACKs only lengthen the upstream estimate; the window minimum removes them. */
static void test_delayed_acks_are_ignored(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50002U, MILLISECOND, 7U);
	uint64_t sent_ns;
	unsigned int packet = 0U;

	memset(&estimator, 0, sizeof(estimator));
	for (sent_ns = 0U; sent_ns < 4000U * MILLISECOND; sent_ns += MILLISECOND) {
		uint64_t ack_delay_ns = packet++ % 5U == 0U ? 0U : 40U * MILLISECOND;

		send_sample(
			&estimator,
			&remote,
			sent_ns,
			0U,
			sent_ns >= 3000U * MILLISECOND ? 60U * MILLISECOND : 0U,
			ack_delay_ns
		);
	}
	result_after(&estimator, sent_ns, &estimate);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 60);
}

/* A changed receiver wait looks like upload queueing while no prompt echoes remain. */
static void test_sustained_ack_wait_change_and_recovery(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50015U, MILLISECOND, 91U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	for (; now < 4000U * MILLISECOND; now += MILLISECOND)
		send_sample(&estimator, &remote, now, 0U, 0U, 40U * MILLISECOND);
	tcpdelay_estimator_result(&estimator, ORIGIN_NS + now - MILLISECOND + PATH_NS, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 40);

	/* Confirm recovery after 300 ms; a prompt minimum may restore zero earlier. */
	now = send_span(&estimator, &remote, now, now + 300U * MILLISECOND, 0U, 0U);
	tcpdelay_estimator_result(&estimator, ORIGIN_NS + now - MILLISECOND + PATH_NS, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 0);
}

/* A constant receiver wait present during calibration is absorbed by its floor. */
static void test_constant_ack_wait_is_baseline(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50016U, MILLISECOND, 101U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	for (now = 0U; now < 4000U * MILLISECOND; now += MILLISECOND)
		send_sample(&estimator, &remote, now, 0U, 0U, 40U * MILLISECOND);
	tcpdelay_estimator_result(&estimator, ORIGIN_NS + now - MILLISECOND + PATH_NS, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 0);
}

static void test_results_expire(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50003U, MILLISECOND, 0U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	result_after(&estimator, now + 150U * MILLISECOND, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	result_after(&estimator, now + 250U * MILLISECOND, &estimate);
	assert(!estimate.download_valid);
	assert(!estimate.upload_valid);
}

/* A rolling floor loses a persistent queue when its two low-delay buckets expire. */
static void test_standing_queue_floor_tracks_two_absolute_phases(uint64_t phase_ns)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50014U, MILLISECOND, 77U);
	const uint64_t path_origin_ns = ORIGIN_NS + PATH_NS;
	const uint64_t expiry_ns = UINT64_C(60000) * MILLISECOND;
	uint64_t now;
	uint64_t end_ns;

	memset(&estimator, 0, sizeof(estimator));
	tcpdelay_estimator_set_policy(&estimator, TCPDELAY_BASELINE_FOLLOW);
	now = send_span(&estimator, &remote, phase_ns, phase_ns + 3000U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	tcpdelay_estimator_result(
		&estimator,
		ORIGIN_NS + now - MILLISECOND + PATH_NS + 80U * MILLISECOND,
		&estimate
	);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 80);
	assert_close(estimate.upload_queue_microseconds, 20);

	/* Both calibration phases are in bucket 0; its second absolute boundary is 60 s. */
	end_ns = expiry_ns - path_origin_ns - 80U * MILLISECOND;
	now = send_span(&estimator, &remote, now, end_ns, 80U * MILLISECOND, 20U * MILLISECOND);
	tcpdelay_estimator_result(
		&estimator,
		ORIGIN_NS + now - MILLISECOND + PATH_NS + 80U * MILLISECOND,
		&estimate
	);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 80);
	assert_close(estimate.upload_queue_microseconds, 20);

	/* FOLLOW keeps the existing rolling behavior after the second bucket. */
	end_ns = expiry_ns + 100U * MILLISECOND - path_origin_ns - 80U * MILLISECOND;
	now = send_span(&estimator, &remote, now, end_ns, 80U * MILLISECOND, 20U * MILLISECOND);
	tcpdelay_estimator_result(
		&estimator,
		ORIGIN_NS + now - MILLISECOND + PATH_NS + 80U * MILLISECOND,
		&estimate
	);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 0);

	/* An empty path lets the rolling floor recover; later congestion is visible again. */
	now = send_span(&estimator, &remote, now, now + 65000U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	tcpdelay_estimator_result(
		&estimator,
		ORIGIN_NS + now - MILLISECOND + PATH_NS + 80U * MILLISECOND,
		&estimate
	);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 80);
	assert_close(estimate.upload_queue_microseconds, 20);
}

static void test_hold_preserves_standing_queue(uint64_t phase_ns)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50017U, MILLISECOND, 113U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, phase_ns, phase_ns + 3000U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 61000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 80);
	assert_close(estimate.upload_queue_microseconds, 20);

	/* The original low-delay floor keeps the same standing queue visible. */
	now = send_span(&estimator, &remote, now, now + 1000U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_microseconds, 80);
	assert_close(estimate.upload_queue_microseconds, 20);
}

static void test_hold_accepts_lower_raw_minimum(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50019U, MILLISECOND, 131U);
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
		3000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	initial_download_floor = estimator.flows[0].download_floor[0].current;
	initial_upload_floor = estimator.flows[0].upload_floor[0].current;
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * MILLISECOND,
		60U * MILLISECOND,
		10U * MILLISECOND
	);
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * MILLISECOND,
		40U * MILLISECOND,
		5U * MILLISECOND
	);

	download_floor = &estimator.flows[0].download_floor[0];
	upload_floor = &estimator.flows[0].upload_floor[0];
	assert(download_floor->current < initial_download_floor);
	assert(download_floor->previous == download_floor->current);
	assert(upload_floor->current < initial_upload_floor);
	assert(upload_floor->previous == upload_floor->current);

	/* Returning to the initial 80/20-ms path shows delay relative to the lower floor. */
	now = send_span(
		&estimator,
		&remote,
		now,
		now + 1000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 40);
	assert_close(estimate.upload_queue_microseconds, 15);
}

static void test_follow_hold_follow_transition(void)
{
	struct tcpdelay_estimator estimator;
	struct remote remote = remote_flow(50018U, MILLISECOND, 127U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(
		&estimator,
		&remote,
		0U,
		3000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	tcpdelay_estimator_set_policy(&estimator, TCPDELAY_BASELINE_FOLLOW);
	now = send_span(
		&estimator,
		&remote,
		now,
		31000U * MILLISECOND,
		100U * MILLISECOND,
		40U * MILLISECOND
	);
	assert(estimator.flows[0].download_floor[0].previous == 0);
	assert(estimator.flows[0].download_floor[0].current == (int64_t)(20U * MILLISECOND));

	/* HOLD retains FOLLOW's older low bucket when its current bucket has risen. */
	tcpdelay_estimator_set_policy(&estimator, TCPDELAY_BASELINE_HOLD);
	now = send_span(
		&estimator,
		&remote,
		now,
		61000U * MILLISECOND,
		140U * MILLISECOND,
		60U * MILLISECOND
	);
	assert(estimator.flows[0].download_floor[0].current == 0);
	assert(estimator.flows[0].download_floor[0].current ==
	       estimator.flows[0].download_floor[0].previous);

	/* With clear evidence, FOLLOW can move the floor up after its rolling window. */
	tcpdelay_estimator_set_policy(&estimator, TCPDELAY_BASELINE_FOLLOW);
	now = send_span(
		&estimator,
		&remote,
		now,
		121000U * MILLISECOND,
		140U * MILLISECOND,
		60U * MILLISECOND
	);
	assert(estimator.flows[0].download_floor[0].current == (int64_t)(60U * MILLISECOND));
	assert(estimator.flows[0].download_floor[0].previous ==
	       estimator.flows[0].download_floor[0].current);
}

static void test_samples_without_departure(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50004U, MILLISECOND, 99U);
	uint64_t now;

	remote.departures = false;
	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	now = send_span(&estimator, &remote, now, now + 500U * MILLISECOND, 25U * MILLISECOND, 0U);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(!estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 25);
}

/* A new flow's congested baseline cannot erase an established flow's queue. */
static void test_new_flow_keeps_established_queues(bool departures)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote established = remote_flow(50008U, MILLISECOND, 1000U);
	struct remote newcomer = remote_flow(50009U, MILLISECOND, 2000U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &established, 0U, 3000U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&established,
		now,
		4000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	newcomer.departures = departures;
	for (; now < 7000U * MILLISECOND; now += MILLISECOND) {
		send_sample(&estimator, &established, now, 80U * MILLISECOND, 20U * MILLISECOND, 0U);
		send_sample(&estimator, &newcomer, now, 80U * MILLISECOND, 20U * MILLISECOND, 0U);
	}
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 80);
	assert_close(estimate.upload_queue_microseconds, 20);

	/* Once A stops, B may supply its own pair, but never A's stale upload. */
	now = send_span(
		&estimator,
		&newcomer,
		now,
		7300U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(estimate.upload_valid == departures);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 0);
}

/* Never synthesize a pair from an older download-only flow and a newer upload. */
static void test_directional_pair_comes_from_one_flow(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote download_only = remote_flow(50010U, MILLISECOND, 1000U);
	struct remote paired = remote_flow(50011U, MILLISECOND, 2000U);
	uint64_t now;

	download_only.departures = false;
	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &download_only, 0U, 1000U * MILLISECOND, 0U, 0U);
	for (; now < 4000U * MILLISECOND; now += MILLISECOND) {
		send_sample(&estimator, &download_only, now, 0U, 0U, 0U);
		send_sample(&estimator, &paired, now, 0U, 0U, 0U);
	}
	for (; now < 4500U * MILLISECOND; now += MILLISECOND) {
		send_sample(&estimator, &download_only, now, 0U, 0U, 0U);
		send_sample(&estimator, &paired, now, 40U * MILLISECOND, 80U * MILLISECOND, 0U);
	}
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 40);
	assert_close(estimate.upload_queue_microseconds, 80);

	/* The surviving partial flow remains available after the pair expires. */
	now = send_span(&estimator, &download_only, now, 5000U * MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(!estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 0);
}

static void test_reordered_packet_is_skipped(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50005U, MILLISECOND, 0xfffffff0U);
	uint64_t now;
	uint64_t ticks;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	ticks = estimator.flows[0].ticks;
	/* Sent 20 ms ago, so its TSval is older than the last one seen. */
	send_sample(&estimator, &remote, now - 20U * MILLISECOND, 0U, 0U, 0U);
	assert(estimator.flows[0].ticks == ticks);
	now = send_span(&estimator, &remote, now, now + 500U * MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 0);
}

static void test_later_arrival_with_older_timestamp_is_ignored(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_sample sample;
	struct remote remote = remote_flow(50012U, MILLISECOND, 1000U);
	struct tcpdelay_flow before;

	memset(&estimator, 0, sizeof(estimator));
	send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	memcpy(&before, &estimator.flows[0], sizeof(before));
	sample.flow = before.key;
	/* send_span is end-exclusive, so the last sent time is 2999 ms. */
	sample.arrival_ns = ORIGIN_NS + 3000U * MILLISECOND + PATH_NS;
	sample.departure_ns = 0U;
	sample.tsval = before.last_tsval - 1U;
	tcpdelay_estimator_add(&estimator, &sample);

	assert(memcmp(&estimator.flows[0], &before, sizeof(before)) == 0);
}

static void test_earlier_arrival_with_newer_timestamp_is_ignored(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_sample sample;
	struct remote remote = remote_flow(50013U, MILLISECOND, 1000U);
	struct tcpdelay_flow before;

	memset(&estimator, 0, sizeof(estimator));
	send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	memcpy(&before, &estimator.flows[0], sizeof(before));
	sample.flow = before.key;
	/* One millisecond before the last packet produced by the end-exclusive span. */
	sample.arrival_ns = ORIGIN_NS + 2998U * MILLISECOND + PATH_NS;
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
		sample.arrival_ns = ORIGIN_NS + index * MILLISECOND;
		sample.tsval = (uint32_t)(100U + index);
		tcpdelay_estimator_add(&estimator, &sample);
	}

	/* Coarse remote clocks repeat TSval; a later arrival still refreshes LRU. */
	sample.flow = estimator.flows[0].key;
	sample.arrival_ns = ORIGIN_NS + TCPDELAY_FLOWS * MILLISECOND;
	sample.tsval = estimator.flows[0].last_tsval;
	tcpdelay_estimator_add(&estimator, &sample);

	sample.flow.local_port = (uint16_t)(41000U + TCPDELAY_FLOWS);
	sample.arrival_ns += MILLISECOND;
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
	struct remote remote = remote_flow(50006U, 2U * MILLISECOND, 0U);
	uint64_t now;

	memset(&estimator, 0, sizeof(estimator));
	now = send_span(&estimator, &remote, 0U, 5000U * MILLISECOND, 0U, 0U);
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
		sample.arrival_ns = ORIGIN_NS + index * MILLISECOND;
		sample.tsval = (uint32_t)(100U + index);
		tcpdelay_estimator_add(&estimator, &sample);
	}

	/* This reordered record arrives later but must not keep slot 0 active. */
	sample.flow = estimator.flows[0].key;
	sample.arrival_ns = ORIGIN_NS + TCPDELAY_FLOWS * MILLISECOND;
	sample.tsval = estimator.flows[0].last_tsval - 1U;
	tcpdelay_estimator_add(&estimator, &sample);

	sample.flow.local_port = (uint16_t)(40000U + TCPDELAY_FLOWS);
	sample.arrival_ns += MILLISECOND;
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

int main(void)
{
	test_no_estimate_before_the_tick_is_known();
	test_queue_attributed_to_its_direction(MILLISECOND, 1000U);
	test_queue_attributed_to_its_direction(4U * MILLISECOND, 1000U);
	test_queue_attributed_to_its_direction(10U * MILLISECOND, 1000U);
	/* TSval wraps about 0.2 s into the run. */
	test_queue_attributed_to_its_direction(MILLISECOND, 0xffffff00U);
	test_queue_built_before_the_tick_is_known();
	test_delayed_acks_are_ignored();
	test_sustained_ack_wait_change_and_recovery();
	test_constant_ack_wait_is_baseline();
	test_results_expire();
	test_standing_queue_floor_tracks_two_absolute_phases(2000U * MILLISECOND);
	test_standing_queue_floor_tracks_two_absolute_phases(22000U * MILLISECOND);
	test_hold_preserves_standing_queue(2000U * MILLISECOND);
	test_hold_preserves_standing_queue(22000U * MILLISECOND);
	test_hold_accepts_lower_raw_minimum();
	test_follow_hold_follow_transition();
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
