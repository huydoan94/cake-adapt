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
static void send_at(struct tcpdelay_estimator *estimator, const struct remote *remote,
		    uint64_t sent_ns, uint64_t download_queue_ns, uint64_t upload_queue_ns,
		    uint64_t ack_delay_ns)
{
	struct tcpdelay_sample sample = {
		.flow = remote->key,
		.arrival_ns = ORIGIN_NS + sent_ns + PATH_NS + download_queue_ns,
		.tsval = remote->tsval_base + (uint32_t)(sent_ns / remote->tick_ns)
	};

	if (remote->departures) {
		sample.departure_ns =
			ORIGIN_NS + sent_ns - ack_delay_ns - upload_queue_ns - PATH_NS;
	}
	tcpdelay_estimator_add(estimator, &sample);
}

/* One packet per millisecond from start_ns until end_ns. */
static uint64_t send_span(struct tcpdelay_estimator *estimator, const struct remote *remote,
			  uint64_t start_ns, uint64_t end_ns, uint64_t download_queue_ns,
			  uint64_t upload_queue_ns)
{
	uint64_t sent_ns;

	for (sent_ns = start_ns; sent_ns < end_ns; sent_ns += MILLISECOND)
		send_at(estimator, remote, sent_ns, download_queue_ns, upload_queue_ns, 0U);
	return end_ns;
}

static int64_t milliseconds(int64_t microseconds)
{
	return (microseconds + 500) / 1000;
}

/* Result at the arrival time of the last packet sent before sent_ns. */
static void result_after(const struct tcpdelay_estimator *estimator, uint64_t sent_ns,
			 struct tcpdelay_estimate *estimate)
{
	tcpdelay_estimator_result(estimator, ORIGIN_NS + sent_ns + PATH_NS, estimate);
}

static void assert_close(int64_t microseconds, int64_t expected_milliseconds)
{
	int64_t difference = milliseconds(microseconds) - expected_milliseconds;

	if (difference < -2 || difference > 2) {
		fprintf(stderr, "queue %lld us, expected %lld ms\n", (long long)microseconds,
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

	tcpdelay_estimator_init(&estimator);
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

	tcpdelay_estimator_init(&estimator);
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

	now = send_span(&estimator, &remote, now, now + 1000U * MILLISECOND, 30U * MILLISECOND,
			120U * MILLISECOND);
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

	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &remote, 0U, 100U * MILLISECOND, 0U, 0U);
	now = send_span(&estimator, &remote, now, 3000U * MILLISECOND, 20U * MILLISECOND,
			150U * MILLISECOND);
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

	tcpdelay_estimator_init(&estimator);
	for (sent_ns = 0U; sent_ns < 4000U * MILLISECOND; sent_ns += MILLISECOND) {
		uint64_t ack_delay_ns = packet++ % 5U == 0U ? 0U : 40U * MILLISECOND;

		send_at(&estimator, &remote, sent_ns, 0U,
			sent_ns >= 3000U * MILLISECOND ? 60U * MILLISECOND : 0U, ack_delay_ns);
	}
	result_after(&estimator, sent_ns, &estimate);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 60);
}

static void test_results_expire(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50003U, MILLISECOND, 0U);
	uint64_t now;

	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	result_after(&estimator, now + 150U * MILLISECOND, &estimate);
	assert(estimate.download_valid && estimate.upload_valid);
	result_after(&estimator, now + 250U * MILLISECOND, &estimate);
	assert(!estimate.download_valid);
	assert(!estimate.upload_valid);
}

static void test_samples_without_departure(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50004U, MILLISECOND, 99U);
	uint64_t now;

	remote.departures = false;
	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	now = send_span(&estimator, &remote, now, now + 500U * MILLISECOND, 25U * MILLISECOND, 0U);
	result_after(&estimator, now, &estimate);
	assert(estimate.download_valid);
	assert(!estimate.upload_valid);
	assert_close(estimate.download_queue_microseconds, 25);
}

static void test_reordered_packet_is_skipped(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50005U, MILLISECOND, 0xfffffff0U);
	uint64_t now;
	uint64_t ticks;

	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	ticks = estimator.flows[0].ticks;
	/* Sent 20 ms ago, so its TSval is older than the last one seen. */
	send_at(&estimator, &remote, now - 20U * MILLISECOND, 0U, 0U, 0U);
	assert(estimator.flows[0].ticks == ticks);
	now = send_span(&estimator, &remote, now, now + 500U * MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert_close(estimate.download_queue_microseconds, 0);
	assert_close(estimate.upload_queue_microseconds, 0);
}

/* An unknown clock rate is never adopted; that flow gives no estimate. */
static void test_nonstandard_tick_is_rejected(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_estimate estimate;
	struct remote remote = remote_flow(50006U, 2U * MILLISECOND, 0U);
	uint64_t now;

	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &remote, 0U, 5000U * MILLISECOND, 0U, 0U);
	result_after(&estimator, now, &estimate);
	assert(estimator.flows[0].tick_ns == 0U);
	assert(!estimate.download_valid);
}

/* A full table replaces the flow seen least recently. */
static void test_least_recent_flow_is_replaced(void)
{
	struct tcpdelay_estimator estimator;
	struct tcpdelay_sample sample;
	size_t index;
	bool newest = false;
	bool oldest = false;

	tcpdelay_estimator_init(&estimator);
	memset(&sample, 0, sizeof(sample));
	for (index = 0U; index <= TCPDELAY_FLOWS; index++) {
		sample.flow.local_port = (uint16_t)(40000U + index);
		sample.arrival_ns = ORIGIN_NS + index * MILLISECOND;
		tcpdelay_estimator_add(&estimator, &sample);
	}
	for (index = 0U; index < TCPDELAY_FLOWS; index++) {
		assert(estimator.flows[index].used);
		oldest |= estimator.flows[index].key.local_port == 40000U;
		newest |= estimator.flows[index].key.local_port == 40000U + TCPDELAY_FLOWS;
	}
	assert(newest);
	assert(!oldest);
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
	test_results_expire();
	test_samples_without_departure();
	test_reordered_packet_is_skipped();
	test_nonstandard_tick_is_rejected();
	test_least_recent_flow_is_replaced();
	puts("tcpdelay estimator tests passed");
	return 0;
}
