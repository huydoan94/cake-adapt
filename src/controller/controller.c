#define _POSIX_C_SOURCE 200809L

#include "controller/controller.h"
#include "controller/direction.h"
#include "common/utils.h"
#include "constants/controller.h"
#include "constants/units.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int initialize_direction(
	struct controller_direction *direction,
	const struct controller_direction_config *config,
	unsigned int delay_window
)
{
	/* controller_init has already zeroed both directions. */
	direction->config = *config;
	direction->delay_samples = calloc(delay_window, sizeof(*direction->delay_samples));
	if (direction->delay_samples == NULL)
		return -1;
	direction->delayed_samples = calloc(delay_window, sizeof(*direction->delayed_samples));
	if (direction->delayed_samples == NULL)
		return -1;
	direction->state = CONTROLLER_LINE_UNKNOWN;
	direction->congestion = CONTROLLER_CONGESTION_UNKNOWN;
	direction->shaper_rate_bps = config->base_rate_bps;
	direction->initial_rate_pending = config->adjust;
	/* Without attribution, every detected bufferbloat may cut this direction. */
	direction->bufferbloat_attributed = true;
	return 0;
}

/* ratio_of() multiplies a remainder below one million by the factor. */
static bool factor_valid(uint64_t ratio_e6)
{
	return ratio_e6 <= UINT64_MAX / RATIO_ONE_E6;
}

int controller_init(struct controller *controller, const struct controller_config *config)
{
	memset(controller, 0, sizeof(*controller));
	if (config->bufferbloat_detection_window == 0U ||
	    config->bufferbloat_detection_threshold > config->bufferbloat_detection_window ||
	    !factor_valid(config->rate_minimum_adjust_down_bufferbloat_ratio_e6) ||
	    !factor_valid(config->rate_maximum_adjust_down_bufferbloat_ratio_e6) ||
	    !factor_valid(config->rate_minimum_adjust_up_high_load_ratio_e6) ||
	    !factor_valid(config->rate_maximum_adjust_up_high_load_ratio_e6) ||
	    !factor_valid(config->rate_adjust_down_low_load_ratio_e6) ||
	    !factor_valid(config->rate_adjust_up_low_load_ratio_e6)) {
		errno = EINVAL;
		return -1;
	}

	controller->config = *config;
	if (initialize_direction(
		    &controller->download,
		    &config->download,
		    config->bufferbloat_detection_window
	    ) != 0 ||
	    initialize_direction(
		    &controller->upload,
		    &config->upload,
		    config->bufferbloat_detection_window
	    ) != 0) {
		/* The controller was zeroed above, so unallocated arrays are NULL. */
		controller_close(controller);
		return -1;
	}
	return 0;
}

static void close_direction(struct controller_direction *direction)
{
	free(direction->delay_samples);
	free(direction->delayed_samples);
	direction->delay_samples = NULL;
	direction->delayed_samples = NULL;
}

void controller_close(struct controller *controller)
{
	close_direction(&controller->download);
	close_direction(&controller->upload);
}

static void update_direction(
	struct controller_direction *direction,
	const struct controller *controller,
	const struct direction_update *update,
	struct controller_direction_output *output
)
{
	enum controller_line_state previous_state = direction->state;
	enum controller_congestion_state previous_congestion = direction->congestion;
	bool attributed = update->bufferbloat_attributed;
	bool ack_share_active = update->ceiling_bps != UINT64_MAX;

	output->state = congestion_update_line_state(direction, update->input);
	output->congestion = congestion_update(direction, &controller->config, &update->latency);
	output->average_delay_us = direction->average_delay_us;
	rate_set_output(
		direction,
		update->input,
		rate_adjust(direction, controller, update),
		output
	);

	output->delayed_sample_count = direction->delayed_sample_count;
	output->state_changed = output->state != previous_state;
	output->congestion_changed = output->congestion != previous_congestion;
	output->bufferbloat_attributed = attributed;
	output->bufferbloat_attribution_changed = attributed != direction->bufferbloat_attributed;
	direction->bufferbloat_attributed = attributed;
	output->ack_share_active = ack_share_active;
	output->ack_share_ceiling_bps = update->ceiling_bps;
	output->ack_share_changed = ack_share_active != direction->ack_share_active;
	direction->ack_share_active = ack_share_active;
}

void controller_update(
	struct controller *controller,
	const struct controller_input *input,
	struct controller_output *output
)
{
	const struct controller_config *config = &controller->config;
	uint64_t download_delivery_e6 =
		coupling_delivery_ratio_e6(&controller->download, &input->download);
	const struct controller_queue_input *queue = &input->queue;
	int64_t total_queue_us = queue->download_us + queue->upload_us;
	struct direction_update download = {
		.input = &input->download,
		.latency = input->download_latency,
		/*
		 * With one shared delay, download delivering its full shaper rate has
		 * no standing queue, so the delay is upload's; download loaded but
		 * delivering less than its shaper rate is the bottleneck, so the delay
		 * is its own.
		 */
		.bufferbloat_attributed = !config->shared_delay ||
					  download_delivery_e6 < FULL_DELIVERY_RATIO_E6,
		.ceiling_bps = coupling_download_ceiling_bps(controller, input),
		.timestamp_us = input->timestamp_us,
	};
	struct direction_update upload = {
		.input = &input->upload,
		.latency = input->upload_latency,
		.bufferbloat_attributed = !config->shared_delay ||
					  download_delivery_e6 <=
						  config->high_load_threshold_ratio_e6 ||
					  download_delivery_e6 >= FULL_DELIVERY_RATIO_E6,
		.ceiling_bps = UINT64_MAX,
		.timestamp_us = input->timestamp_us,
	};

	/*
	 * Measured per-direction queues replace the heuristic, unless they are too
	 * small to explain the shared delay, which then arose outside the paths
	 * TCP observes.
	 */
	if (config->shared_delay && queue->valid &&
	    total_queue_us >= QUEUE_ATTRIBUTION_MINIMUM_US) {
		download.bufferbloat_attributed =
			coupling_holds_queue_share(queue->download_us, total_queue_us);
		upload.bufferbloat_attributed =
			coupling_holds_queue_share(queue->upload_us, total_queue_us);
		/*
		 * Split the round-trip delta by the measured shares instead of RTT/2
		 * each way, so a one-sided queue counts at its full size.
		 */
		if (download.latency.valid && upload.latency.valid) {
			int64_t round_trip_us =
				download.latency.owd_delta_us + upload.latency.owd_delta_us;

			download.latency.owd_delta_us = signed_rounded_divide(
				round_trip_us * queue->download_us,
				total_queue_us
			);
			upload.latency.owd_delta_us = round_trip_us - download.latency.owd_delta_us;
		}
	}

	update_direction(&controller->download, controller, &download, &output->download);
	update_direction(&controller->upload, controller, &upload, &output->upload);
}

void controller_set_minimum_rates(struct controller *controller, uint64_t timestamp_us)
{
	struct controller_direction *directions[] = { &controller->download, &controller->upload };
	size_t index;

	for (index = 0U; index < ARRAY_SIZE(directions); index++) {
		struct controller_direction *direction = directions[index];
		const struct controller_direction_config *limits = &direction->config;

		if (!limits->adjust)
			continue;
		direction->shaper_rate_bps = limits->minimum_rate_bps;
		direction->last_congestion_adjustment_us = timestamp_us;
		direction->last_decay_adjustment_us = timestamp_us;
		direction->initial_rate_pending = false;
	}
}
