/* Reuse the deterministic TCP timestamp source, not production expected values. */
#define main estimator_test_main
#include "../../tests/tcpdelay/test_estimator.c"
#undef main

static void
print_estimate(const char *label, const struct tcpdelay_estimator *estimator, uint64_t sent_ns)
{
	struct tcpdelay_estimate estimate;

	tcpdelay_estimator_result(estimator, ORIGIN_NS + sent_ns + PATH_NS, &estimate);
	printf("%s: dl=%lld us (%d), ul=%lld us (%d)\n",
	       label,
	       (long long)estimate.download_queue_microseconds,
	       estimate.download_valid,
	       (long long)estimate.upload_queue_microseconds,
	       estimate.upload_valid);
}

static void newcomer_case(bool departures)
{
	struct tcpdelay_estimator estimator;
	struct remote established = remote_flow(50008U, MILLISECOND, 1000U);
	struct remote newcomer = remote_flow(50009U, MILLISECOND, 1000U);
	uint64_t now;

	newcomer.departures = departures;
	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &established, 0U, 3000U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&established,
		now,
		4000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	print_estimate("established alone", &estimator, now);
	for (; now < 7000U * MILLISECOND; now += MILLISECOND) {
		send_sample(&estimator, &established, now, 80U * MILLISECOND, 20U * MILLISECOND, 0U);
		send_sample(&estimator, &newcomer, now, 80U * MILLISECOND, 20U * MILLISECOND, 0U);
	}
	print_estimate(
		departures ? "newcomer has both directions" : "newcomer has download only",
		&estimator,
		now
	);
	now = send_span(
		&estimator,
		&newcomer,
		now,
		7300U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	print_estimate("established stops; newcomer remains", &estimator, now);
	print_estimate("all samples expired", &estimator, now + 1000U * MILLISECOND);
}

int main(void)
{
	printf("flow_bytes=%zu estimator_bytes=%zu\n",
	       sizeof(struct tcpdelay_flow),
	       sizeof(struct tcpdelay_estimator));
	newcomer_case(false);
	newcomer_case(true);
	return 0;
}
