#include "common/constants.h"
#define main existing_controller_tests
#include "tests/controller/test_controller.c"
#undef main

/* Download is loaded (81% of 8 Mbit/s) and fping sees 50 ms each way. */
static uint64_t run(bool tcp_estimate)
{
	struct controller_config config = adjusting_config();
	struct controller controller;
	struct controller_output output;
	struct controller_input input =
		input_with_rates(6500U * KILOBIT, 8U * MEBABIT, 1U * MEBABIT, 8U * MEBABIT);
	unsigned int step;

	config.shared_delay = true;
	init_controller(&controller, &config);
	set_latency_delta(&input, 50000);
	if (tcp_estimate)
		input.queue = (struct controller_queue_input){ .valid = true,
							       .download_microseconds = 0,
							       .upload_microseconds = 20000 };
	for (step = 0U; step < 40U; step++) {
		input.timestamp_microseconds += 500000U;
		input.download.traffic_sample_id++;
		input.upload.traffic_sample_id++;
		controller_update(&controller, &input, &output);
		accept_rates(&input, &output);
		printf("%s %u %llu\n", tcp_estimate ? "tcp" : "heur", step, (unsigned long long)(input.download.cake_rate_bits_per_second / KILOBIT));
	}
	controller_close(&controller);
	return input.download.cake_rate_bits_per_second;
}

int main(void)
{
	printf("download shaper after 20 s, heuristic only:        %llu kbit/s\n",
	       (unsigned long long)(run(false) / KILOBIT));
	printf("download shaper after 20 s, TCP says dl=0 ul=20ms: %llu kbit/s\n",
	       (unsigned long long)(run(true) / KILOBIT));
	return 0;
}
