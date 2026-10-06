/* Reuse the deterministic timestamp generator, with the selected source tree. */
#include <inttypes.h>
#define main estimator_test_main
#include "tcpdelay/test_estimator.c"
#undef main

int main(void)
{
	struct tcpdelay_estimator estimator;
	const struct remote remote = remote_flow(1U, MILLISECOND, 1000U);
	struct tcpdelay_estimate estimate;

	tcpdelay_estimator_init(&estimator);
	printf("estimator_bytes=%zu\n", sizeof(estimator));
	send_span(&estimator, &remote, 0U, 3000U * MILLISECOND, 0U, 0U);
	send_span(
		&estimator,
		&remote,
		3000U * MILLISECOND,
		70000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	tcpdelay_estimator_result(
		&estimator,
		ORIGIN_NS + 69999U * MILLISECOND + PATH_NS + 80U * MILLISECOND,
		&estimate
	);
	printf("standing: dl_valid=%d dl_us=%" PRId64 " ul_valid=%d ul_us=%" PRId64 "\n",
	       estimate.download_valid,
	       estimate.download_queue_microseconds,
	       estimate.upload_valid,
	       estimate.upload_queue_microseconds);
	return 0;
}
