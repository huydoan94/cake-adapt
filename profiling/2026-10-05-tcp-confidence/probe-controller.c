/* Reuse the existing controller fixture and allocation wrappers. */
#include <inttypes.h>
#define main controller_test_main
#include "controller/test_controller.c"
#undef main

int main(void)
{
	struct controller controller;
	struct controller_config config = adjusting_config();
	struct controller_input input =
		input_with_rates(7U * MEBABIT, 8U * MEBABIT, 8U * MEBABIT, 8U * MEBABIT);
	struct controller_output output;

	config.shared_delay = true;
	input.queue = measured_queue(0, 40000);
	init_controller(&controller, &config);
	detect_congestion(&controller, &input, &output);
	printf("ack-conflict: dl_attributed=%d dl_rate=%" PRIu64
	       " ul_attributed=%d ul_rate=%" PRIu64 "\n",
	       output.download.bufferbloat_attributed,
	       output.download.rate_bits_per_second,
	       output.upload.bufferbloat_attributed,
	       output.upload.rate_bits_per_second);
	controller_close(&controller);
	return 0;
}
