#define _POSIX_C_SOURCE 200809L

/*
 * Replays a cake-autorate processing trace through the controller and checks
 * it against every recorded decision. Delay counts and the bufferbloat flags
 * must match exactly. cake-adapt rounds the average delay to the nearest
 * microsecond where cake-autorate truncates it, so the averages may differ by
 * AVERAGE_TOLERANCE_US. cake-adapt does not truncate rates to
 * whole kbit/s as cake-autorate's Bash integers do, so shaper rates may differ
 * within RATE_TOLERANCE_PER_MILLION and the serialization-compensated
 * thresholds, which follow the rate, within THRESHOLD_TOLERANCE_US.
 */
#include "controller/controller.h"
#include "common/utils.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIRE_PACKET_BITS 12000U
#define REPORT_LIMIT 10U
/* Measured worst cases on both traces: 0.119% and 2 us. */
#define RATE_TOLERANCE_PER_MILLION 5000U
#define THRESHOLD_TOLERANCE_US 3U
#define AVERAGE_TOLERANCE_US 1U

static uint64_t worst_rate_per_million;
static uint64_t worst_threshold_us;

static uint64_t difference(uint64_t first, uint64_t second)
{
	return first > second ? first - second : second - first;
}

/* Within tolerance; also records the worst difference seen. */
static bool rate_close(uint64_t rate_bps, uint64_t expected_kbps)
{
	uint64_t expected = expected_kbps * KILOBIT;
	uint64_t per_million = difference(rate_bps, expected) * 1000000U / expected;

	if (per_million > worst_rate_per_million)
		worst_rate_per_million = per_million;
	return per_million <= RATE_TOLERANCE_PER_MILLION;
}

static bool average_close(int64_t average, int64_t expected)
{
	return absolute_difference(average, expected) <= AVERAGE_TOLERANCE_US;
}

static bool threshold_close(uint64_t threshold, uint64_t expected)
{
	uint64_t gap = difference(threshold, expected);

	if (gap > worst_threshold_us)
		worst_threshold_us = gap;
	return gap <= THRESHOLD_TOLERANCE_US;
}

struct trace_sample {
	uint64_t processed_us;
	uint64_t achieved_kbps[2];
	int64_t delta_us[2];
	uint64_t delay_threshold[2];
	uint64_t adjust_up_threshold[2];
	uint64_t adjust_down_threshold[2];
	unsigned int sum_delays[2];
	int64_t average_delta[2];
	int bufferbloat[2];
	uint64_t rate_kbps[2];
};

/*
 * cake-autorate ac75f49 defaults.sh with the trace's rate limits. cake-adapt's
 * own improvements are switched off, so this replay checks that the rest of the
 * controller still decides exactly as upstream does.
 */
static struct controller_config upstream_config(void)
{
	const struct controller_direction_config direction = {
		.adjust = true,
		.minimum_rate_bps = 10000U * KILOBIT,
		.base_rate_bps = 20000U * KILOBIT,
		.maximum_rate_bps = 50000U * KILOBIT,
		.average_delay_maximum_adjust_up_us = 10000U,
		.delay_threshold_us = 30000U,
		.average_delay_maximum_adjust_down_us = 60000U,
	};

	return (struct controller_config){ .download = direction,
					   .upload = direction,
					   .bufferbloat_detection_window = 6U,
					   .bufferbloat_detection_threshold = 3U,
					   .rate_minimum_adjust_down_bufferbloat_ratio_e6 = 990000U,
					   .rate_maximum_adjust_down_bufferbloat_ratio_e6 = 750000U,
					   .rate_minimum_adjust_up_high_load_ratio_e6 = 1000000U,
					   .rate_maximum_adjust_up_high_load_ratio_e6 = 1040000U,
					   .rate_adjust_down_low_load_ratio_e6 = 990000U,
					   .rate_adjust_up_low_load_ratio_e6 = 1010000U,
					   .high_load_threshold_ratio_e6 = 750000U,
					   .bufferbloat_refractory_period_us = 300000U,
					   .decay_refractory_period_us = 1000000U,
					   .shared_delay = false };
}

static bool parse_sample(const char *line, struct trace_sample *sample)
{
	return sscanf(line,
		      "D %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNd64 " %" SCNd64 " %" SCNu64
		      " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %u %" SCNd64
		      " %u %" SCNd64 " %d %d %" SCNu64 " %" SCNu64,
		      &sample->processed_us,
		      &sample->achieved_kbps[0],
		      &sample->achieved_kbps[1],
		      &sample->delta_us[0],
		      &sample->delta_us[1],
		      &sample->delay_threshold[0],
		      &sample->adjust_up_threshold[0],
		      &sample->adjust_down_threshold[0],
		      &sample->delay_threshold[1],
		      &sample->adjust_up_threshold[1],
		      &sample->adjust_down_threshold[1],
		      &sample->sum_delays[0],
		      &sample->average_delta[0],
		      &sample->sum_delays[1],
		      &sample->average_delta[1],
		      &sample->bufferbloat[0],
		      &sample->bufferbloat[1],
		      &sample->rate_kbps[0],
		      &sample->rate_kbps[1]) == 19;
}

/* Returns the number of decisions that differ from cake-autorate's. */
static unsigned int replay(const char *path)
{
	static const char *const names[] = { "download", "upload" };
	struct controller controller;
	const struct controller_config config = upstream_config();
	FILE *trace = fopen(path, "r");
	char line[512];
	uint64_t sample_id = 0U;
	uint64_t cake_kbps[2] = { 20000U, 20000U };
	unsigned int samples = 0U;
	unsigned int mismatches = 0U;

	assert(trace != NULL);
	assert(controller_init(&controller, &config) == 0);
	/* The monitor compensates before the first sample, once CAKE is discovered. */
	controller_set_serialization_compensation(&controller, WIRE_PACKET_BITS, WIRE_PACKET_BITS);
	while (fgets(line, sizeof(line), trace) != NULL) {
		struct trace_sample sample;
		struct controller_input input;
		struct controller_output output;
		const struct controller_direction_output *outputs[2];
		const struct controller_direction *directions[2];
		size_t index;

		if (line[0] == 'L') {
			sample_id++;
			continue;
		}
		if (line[0] != 'D')
			continue;
		assert(parse_sample(line, &sample));
		samples++;
		input = (struct controller_input){
			.download = { .valid = true,
				      .traffic_sample_id = sample_id,
				      .traffic_rate_bps =
					      sample.achieved_kbps[0] * KILOBIT,
				      .cake_rate_bps = cake_kbps[0] * KILOBIT, },
			.upload = { .valid = true,
				    .traffic_sample_id = sample_id,
				    .traffic_rate_bps =
					    sample.achieved_kbps[1] * KILOBIT,
				    .cake_rate_bps = cake_kbps[1] * KILOBIT, },
			.download_latency = { .valid = true,
					      .owd_delta_us =
						      sample.delta_us[0], },
			.upload_latency = { .valid = true,
					    .owd_delta_us =
						    sample.delta_us[1], },
			.timestamp_us = sample.processed_us
		};
		controller_update(&controller, &input, &output);
		controller_set_serialization_compensation(
			&controller,
			WIRE_PACKET_BITS,
			WIRE_PACKET_BITS
		);
		outputs[0] = &output.download;
		outputs[1] = &output.upload;
		directions[0] = &controller.download;
		directions[1] = &controller.upload;
		for (index = 0U; index < 2U; index++) {
			const struct controller_direction_output *decision = outputs[index];
			const struct controller_direction_config *compensated =
				&directions[index]->config;
			uint64_t rate_kbps = directions[index]->shaper_rate_bps / KILOBIT;
			bool bufferbloat = decision->congestion == CONTROLLER_CONGESTION_DETECTED;

			bool rate_matches = rate_close(
				directions[index]->shaper_rate_bps,
				sample.rate_kbps[index]
			);
			bool thresholds_match =
				threshold_close(
					compensated->delay_threshold_us,
					sample.delay_threshold[index]
				) &
				threshold_close(
					compensated->average_delay_maximum_adjust_up_us,
					sample.adjust_up_threshold[index]
				) &
				threshold_close(
					compensated->average_delay_maximum_adjust_down_us,
					sample.adjust_down_threshold[index]
				);

			if (!rate_matches || !thresholds_match ||
			    decision->delayed_sample_count != sample.sum_delays[index] ||
			    !average_close(decision->average_delay_us, sample.average_delta[index]) ||
			    bufferbloat != (sample.bufferbloat[index] != 0)) {
				if (mismatches < REPORT_LIMIT) {
					printf("%s sample %u %s: rate %" PRIu64 "/%" PRIu64
					       " sum %u/%u avg %" PRId64 "/%" PRId64 " bb %d/%d"
					       " thr %" PRIu64 "/%" PRIu64 " up %" PRIu64
					       "/%" PRIu64 " down %" PRIu64 "/%" PRIu64
					       " (cake-adapt/cake-autorate)\n",
					       path,
					       samples,
					       names[index],
					       rate_kbps,
					       sample.rate_kbps[index],
					       decision->delayed_sample_count,
					       sample.sum_delays[index],
					       decision->average_delay_us,
					       sample.average_delta[index],
					       bufferbloat,
					       sample.bufferbloat[index],
					       compensated->delay_threshold_us,
					       sample.delay_threshold[index],
					       compensated->average_delay_maximum_adjust_up_us,
					       sample.adjust_up_threshold[index],
					       compensated->average_delay_maximum_adjust_down_us,
					       sample.adjust_down_threshold[index]);
				}
				mismatches++;
			}
			/* CAKE holds whatever rate cake-autorate set for this sample. */
			cake_kbps[index] = sample.rate_kbps[index];
		}
	}
	assert(fclose(trace) == 0);
	controller_close(&controller);
	printf("%s: replayed %u samples, %u mismatching decisions\n", path, samples, mismatches);
	return mismatches;
}

int main(void)
{
	static const char *const traces[] = {
		"controller/fixtures/cake-autorate-ac75f49-ab.trace",
		"controller/fixtures/cake-autorate-ac75f49-congestion.trace",
	};
	unsigned int mismatches = 0U;
	size_t index;

	for (index = 0U; index < sizeof(traces) / sizeof(traces[0]); index++)
		mismatches += replay(traces[index]);
	printf("worst rate difference %" PRIu64 " per million, worst threshold difference %" PRIu64
	       " us\n",
	       worst_rate_per_million,
	       worst_threshold_us);
	assert(mismatches == 0U);
	(void)puts("cake-autorate replay tests passed");
	return 0;
}
