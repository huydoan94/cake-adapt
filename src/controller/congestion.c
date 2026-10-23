#include "controller/direction.h"
#include "common/utils.h"
#include "constants/controller.h"

#include <stdint.h>

static void reset_line_state(struct controller_direction *direction)
{
	direction->state = CONTROLLER_LINE_UNKNOWN;
	direction->saturation_samples = 0U;
	direction->recovery_samples = 0U;
}

enum controller_line_state congestion_update_line_state(
	struct controller_direction *direction,
	const struct controller_direction_input *input
)
{
	if (!input->valid || input->cake_rate_bps == 0U) {
		reset_line_state(direction);
		return direction->state;
	}

	if (direction->state == CONTROLLER_LINE_SATURATED) {
		uint64_t recovery_threshold_bps =
			ratio_of_rounded_up(input->cake_rate_bps, SATURATION_EXIT_RATIO_E6);

		direction->saturation_samples = 0U;
		if (input->traffic_rate_bps <= recovery_threshold_bps) {
			direction->recovery_samples++;
			if (direction->recovery_samples >= RECOVERY_CONFIRMATION_SAMPLES) {
				direction->state = CONTROLLER_LINE_BELOW_CAPACITY;
				direction->recovery_samples = 0U;
			}
		} else {
			direction->recovery_samples = 0U;
		}

		return direction->state;
	}

	direction->recovery_samples = 0U;
	/* Only the threshold for the current hysteresis state is needed. */
	if (input->traffic_rate_bps >=
	    ratio_of_rounded_up(input->cake_rate_bps, SATURATION_ENTER_RATIO_E6)) {
		direction->saturation_samples++;
		if (direction->saturation_samples >= SATURATION_CONFIRMATION_SAMPLES) {
			direction->state = CONTROLLER_LINE_SATURATED;
			direction->saturation_samples = 0U;
		}
	} else {
		direction->state = CONTROLLER_LINE_BELOW_CAPACITY;
		direction->saturation_samples = 0U;
	}

	return direction->state;
}

enum controller_congestion_state congestion_update(
	struct controller_direction *direction,
	const struct controller_config *config,
	const struct controller_latency_input *latency
)
{
	const struct controller_direction_config *limits = &direction->config;
	int64_t *sample_us;
	unsigned char *delayed;
	unsigned int index;

	if (!latency->valid) {
		direction->congestion = CONTROLLER_CONGESTION_UNKNOWN;
		direction->average_delay_us = 0;
		return direction->congestion;
	}

	index = direction->delay_next_sample;
	sample_us = &direction->delay_samples[index];
	delayed = &direction->delayed_samples[index];

	/* Maintain a fixed rolling window without rescanning every sample. */
	direction->delay_sum_us -= *sample_us;
	direction->delay_sum_us += latency->owd_delta_us;

	/* Preserve the classification made when this sample entered the window. */
	if (*delayed != 0U)
		direction->delayed_sample_count--;
	if (latency->owd_delta_us > 0 &&
	    (uint64_t)latency->owd_delta_us > limits->delay_threshold_us) {
		direction->delayed_sample_count++;
		*delayed = 1U;
	} else {
		*delayed = 0U;
	}
	*sample_us = latency->owd_delta_us;

	direction->delay_next_sample = index + 1U;
	if (direction->delay_next_sample == config->bufferbloat_detection_window)
		direction->delay_next_sample = 0U;
	direction->average_delay_us = signed_rounded_divide(
		direction->delay_sum_us,
		(int64_t)config->bufferbloat_detection_window
	);
	direction->congestion = direction->delayed_sample_count >=
						config->bufferbloat_detection_threshold ?
					CONTROLLER_CONGESTION_DETECTED :
					CONTROLLER_CONGESTION_CLEAR;
	return direction->congestion;
}
