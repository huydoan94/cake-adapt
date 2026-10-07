#define _POSIX_C_SOURCE 200809L

#include "controller/controller.h"
#include "config/defaults.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* rate * factor, down to a rate CAKE can hold. */
static uint64_t scale_rate(uint64_t rate_bits_per_second, uint64_t factor_e6)
{
	uint64_t scaled = ratio_of(rate_bits_per_second, factor_e6);

	return scaled / SHAPER_RATE_STEP_BITS_PER_SECOND * SHAPER_RATE_STEP_BITS_PER_SECOND;
}

static uint64_t clamp_rate(uint64_t rate, const struct controller_direction_config *config)
{
	if (rate < config->minimum_rate_bits_per_second)
		return config->minimum_rate_bits_per_second;
	if (rate > config->maximum_rate_bits_per_second)
		return config->maximum_rate_bits_per_second;
	return rate;
}

static uint64_t
rate_toward_base(uint64_t rate, uint64_t base_rate, const struct controller_config *config)
{
	if (rate > base_rate)
		return max_u64(
			scale_rate(rate, config->rate_adjust_down_low_load_ratio_e6),
			base_rate
		);
	if (rate < base_rate)
		return min_u64(
			scale_rate(rate, config->rate_adjust_up_low_load_ratio_e6),
			base_rate
		);
	return rate;
}

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
	direction->shaper_rate_bits_per_second = config->base_rate_bits_per_second;
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

static void reset_line_state(struct controller_direction *direction)
{
	direction->state = CONTROLLER_LINE_UNKNOWN;
	direction->saturation_samples = 0U;
	direction->recovery_samples = 0U;
}

static enum controller_line_state update_line_state(
	struct controller_direction *direction,
	const struct controller_direction_input *input
)
{
	if (!input->valid || input->cake_rate_bits_per_second == 0U) {
		reset_line_state(direction);
		return direction->state;
	}

	if (direction->state == CONTROLLER_LINE_SATURATED) {
		uint64_t recovery_threshold = ratio_of_rounded_up(
			input->cake_rate_bits_per_second,
			SATURATION_EXIT_RATIO_E6
		);

		direction->saturation_samples = 0U;
		if (input->traffic_rate_bits_per_second <= recovery_threshold) {
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
	if (input->traffic_rate_bits_per_second >=
	    ratio_of_rounded_up(input->cake_rate_bits_per_second, SATURATION_ENTER_RATIO_E6)) {
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

static enum controller_congestion_state update_congestion(
	struct controller_direction *direction,
	const struct controller_config *config,
	const struct controller_latency_input *latency
)
{
	int64_t *sample;
	unsigned char *delayed;
	unsigned int index;

	if (!latency->valid) {
		direction->congestion = CONTROLLER_CONGESTION_UNKNOWN;
		direction->average_delay_us = 0;
		return direction->congestion;
	}

	index = direction->delay_next_sample;
	sample = &direction->delay_samples[index];
	delayed = &direction->delayed_samples[index];

	/* Maintain a fixed rolling window without rescanning every sample. */
	direction->delay_sum_us -= *sample;
	direction->delay_sum_us += latency->owd_delta_us;

	/* Preserve the classification made when this sample entered the window. */
	if (*delayed != 0U)
		direction->delayed_sample_count--;
	if (latency->owd_delta_us > 0 &&
	    (uint64_t)latency->owd_delta_us > direction->config.delay_threshold_us) {
		direction->delayed_sample_count++;
		*delayed = 1U;
	} else {
		*delayed = 0U;
	}
	*sample = latency->owd_delta_us;

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

/* The factor at adjustment between minimum (none) and maximum (RATIO_ONE_E6). */
static uint64_t
interpolate_factor_e6(uint64_t minimum_e6, uint64_t maximum_e6, uint64_t adjustment_e6)
{
	if (maximum_e6 >= minimum_e6)
		return minimum_e6 + ratio_of(maximum_e6 - minimum_e6, adjustment_e6);
	return minimum_e6 - ratio_of(minimum_e6 - maximum_e6, adjustment_e6);
}

/* How far the average delay is from the threshold toward its maximum, as a ratio. */
static uint64_t delay_adjustment_e6(int64_t delay, uint64_t from, uint64_t to)
{
	if (to <= from || (delay > 0 && (uint64_t)delay >= to))
		return RATIO_ONE_E6;
	if (delay <= 0 || (uint64_t)delay <= from)
		return 0U;
	return fraction_to_ratio_e6((uint64_t)delay - from, to - from);
}

static uint64_t downward_factor_e6(
	const struct controller_direction *direction,
	const struct controller_config *config
)
{
	const struct controller_direction_config *thresholds = &direction->config;

	return interpolate_factor_e6(
		config->rate_minimum_adjust_down_bufferbloat_ratio_e6,
		config->rate_maximum_adjust_down_bufferbloat_ratio_e6,
		delay_adjustment_e6(
			direction->average_delay_us,
			thresholds->delay_threshold_us,
			thresholds->average_delay_maximum_adjust_down_us
		)
	);
}

/* Below the threshold, the full increase at the maximum-adjust-up delay and below. */
static uint64_t upward_factor_e6(
	const struct controller_direction *direction,
	const struct controller_config *config
)
{
	const struct controller_direction_config *thresholds = &direction->config;
	uint64_t threshold = thresholds->delay_threshold_us;
	uint64_t full = thresholds->average_delay_maximum_adjust_up_us;
	int64_t delay = direction->average_delay_us;
	uint64_t adjustment_e6;

	if (threshold <= full || delay <= 0 || (uint64_t)delay <= full)
		adjustment_e6 = RATIO_ONE_E6;
	else if ((uint64_t)delay < threshold)
		adjustment_e6 = fraction_to_ratio_e6(threshold - (uint64_t)delay, threshold - full);
	else
		adjustment_e6 = 0U;
	return interpolate_factor_e6(
		config->rate_minimum_adjust_up_high_load_ratio_e6,
		config->rate_maximum_adjust_up_high_load_ratio_e6,
		adjustment_e6
	);
}

/* One direction's share of a controller_update(), after attribution. */
struct direction_update {
	const struct controller_direction_input *input;
	struct controller_latency_input latency;
	/* Bufferbloat is blamed on this direction, so a detected bufferbloat may cut its rate. */
	bool bufferbloat_attributed;
	/* The ACK share ceiling for download; UINT64_MAX when none applies. */
	uint64_t ceiling_bits_per_second;
	uint64_t timestamp_us;
};

static enum controller_rate_reason adjust_rate(
	struct controller_direction *direction,
	const struct controller *controller,
	const struct direction_update *update
)
{
	const struct controller_config *config = &controller->config;
	const struct controller_direction_config *limits = &direction->config;
	const struct controller_direction_input *input = update->input;
	uint64_t timestamp_us = update->timestamp_us;
	uint64_t previous_rate = direction->shaper_rate_bits_per_second;
	bool congested = direction->congestion == CONTROLLER_CONGESTION_DETECTED;
	bool refractory_elapsed;
	bool high_load;
	bool capped = false;

	if (!limits->adjust || !input->valid)
		return CONTROLLER_RATE_UNCHANGED;
	if (direction->initial_rate_pending) {
		direction->initial_rate_pending = false;
		direction->last_congestion_adjustment_us = timestamp_us;
		direction->last_decay_adjustment_us = timestamp_us;
		return CONTROLLER_RATE_INITIAL;
	}
	if (!update->latency.valid)
		return CONTROLLER_RATE_UNCHANGED;

	high_load = load_ratio_e6(input->traffic_rate_bits_per_second, previous_rate) >
		    config->high_load_threshold_ratio_e6;
	/* Both a cut and an increase wait out the bufferbloat refractory period. */
	refractory_elapsed = interval_elapsed(
		timestamp_us,
		direction->last_congestion_adjustment_us,
		config->bufferbloat_refractory_period_us
	);
	if (congested && update->bufferbloat_attributed && refractory_elapsed) {
		direction->shaper_rate_bits_per_second =
			scale_rate(previous_rate, downward_factor_e6(direction, config));
		direction->last_congestion_adjustment_us = timestamp_us;
		/* Do not let low-load decay immediately undo a congestion cut. */
		direction->last_decay_adjustment_us = timestamp_us;
	} else if (!congested && high_load &&
		   input->traffic_sample_id != direction->last_increase_sample_id &&
		   refractory_elapsed) {
		/* Like upstream achieved_rate_updated: one increase per load sample,
		 * even when the factor is one or the maximum rate clips the result. */
		direction->last_increase_sample_id = input->traffic_sample_id;
		direction->shaper_rate_bits_per_second =
			scale_rate(previous_rate, upward_factor_e6(direction, config));
		/* Give the increased rate a full decay interval to be observed. */
		direction->last_decay_adjustment_us = timestamp_us;
	} else if (!congested && !high_load && previous_rate != limits->base_rate_bits_per_second &&
		   interval_elapsed(
			   timestamp_us,
			   direction->last_decay_adjustment_us,
			   config->decay_refractory_period_us
		   )) {
		/* With low load, converge by 1% steps instead of jumping to base. */
		direction->shaper_rate_bits_per_second =
			rate_toward_base(previous_rate, limits->base_rate_bits_per_second, config);
		direction->last_decay_adjustment_us = timestamp_us;
	}

	if (direction->shaper_rate_bits_per_second > update->ceiling_bits_per_second) {
		direction->shaper_rate_bits_per_second = update->ceiling_bits_per_second;
		capped = true;
	}
	/* The minimum rate still wins over the ceiling. */
	direction->shaper_rate_bits_per_second =
		clamp_rate(direction->shaper_rate_bits_per_second, limits);
	if (direction->shaper_rate_bits_per_second == previous_rate)
		return CONTROLLER_RATE_UNCHANGED;
	if (capped)
		return CONTROLLER_RATE_ACK_SHARE;
	if (congested)
		return CONTROLLER_RATE_CONGESTION;
	if (high_load)
		return CONTROLLER_RATE_HIGH_LOAD;
	return CONTROLLER_RATE_RETURN_TO_BASE;
}

static void set_rate_output(
	const struct controller_direction *direction,
	const struct controller_direction_input *input,
	enum controller_rate_reason reason,
	struct controller_direction_output *output
)
{
	if (!direction->config.adjust) {
		output->rate_bits_per_second = input->cake_rate_bits_per_second;
		output->rate_changed = false;
		output->rate_reason = CONTROLLER_RATE_UNCHANGED;
		return;
	}

	output->rate_bits_per_second = direction->shaper_rate_bits_per_second;
	/* Like cake-autorate's first set_shaper_rates(), the base rate is written once
	 * even when CAKE already holds it. */
	output->rate_changed = input->valid &&
			       (reason == CONTROLLER_RATE_INITIAL ||
				input->cake_rate_bits_per_second != output->rate_bits_per_second);
	output->rate_reason = output->rate_changed && reason == CONTROLLER_RATE_UNCHANGED ?
				      CONTROLLER_RATE_RECONCILE :
				      reason;
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
	bool ack_share_active = update->ceiling_bits_per_second != UINT64_MAX;

	output->state = update_line_state(direction, update->input);
	output->congestion = update_congestion(direction, &controller->config, &update->latency);
	output->average_delay_us = direction->average_delay_us;
	set_rate_output(
		direction,
		update->input,
		adjust_rate(direction, controller, update),
		output
	);

	output->delayed_sample_count = direction->delayed_sample_count;
	output->state_changed = output->state != previous_state;
	output->congestion_changed = output->congestion != previous_congestion;
	output->bufferbloat_attributed = attributed;
	output->bufferbloat_attribution_changed = attributed != direction->bufferbloat_attributed;
	direction->bufferbloat_attributed = attributed;
	output->ack_share_active = ack_share_active;
	output->ack_share_ceiling_bits_per_second = update->ceiling_bits_per_second;
	output->ack_share_changed = ack_share_active != direction->ack_share_active;
	direction->ack_share_active = ack_share_active;
}

/*
 * While upload is under high load, ACKs may use what the other traffic leaves
 * free, less a headroom in which that traffic's growth shows, but never less
 * than their minimum share. ACK rate follows download rate, so download is
 * held at the rate whose ACKs fill exactly that allowance.
 */
static uint64_t
download_ceiling(const struct controller *controller, const struct controller_input *input)
{
	const struct controller_config *config = &controller->config;
	const struct controller_ack_input *acks = &input->acks;
	const struct controller_direction_input *download = &input->download;
	const struct controller_direction_input *upload = &input->upload;
	uint64_t room = controller->upload.shaper_rate_bits_per_second;
	uint64_t ack_rate = acks->upload_ack_rate_bits_per_second;
	uint64_t minimum = ratio_of(room, config->ul_congest_ack_share_ratio_e6);
	uint64_t other;
	uint64_t taken;
	uint64_t allowed;

	if (config->ul_congest_ack_share_ratio_e6 == 0U || !acks->valid || ack_rate == 0U ||
	    !upload->valid || !download->valid ||
	    load_ratio_e6(upload->traffic_rate_bits_per_second, room) <=
		    config->high_load_threshold_ratio_e6) {
		return UINT64_MAX;
	}
	other = saturating_sub(acks->upload_rate_bits_per_second, ack_rate);
	taken = other + ratio_of(room, UPLOAD_ACK_HEADROOM_RATIO_E6);
	allowed = saturating_sub(room, taken);
	if (allowed < minimum)
		allowed = minimum;
	if (ack_rate <= allowed)
		return UINT64_MAX;
	/* Down to a rate CAKE can hold, like other rates. */
	return mul_div(download->traffic_rate_bits_per_second, allowed, ack_rate) /
	       SHAPER_RATE_STEP_BITS_PER_SECOND * SHAPER_RATE_STEP_BITS_PER_SECOND;
}

/* A direction holding at least QUEUE_SHARE_RATIO_E6 of a positive total queue. */
static bool holds_queue_share(int64_t queue_us, int64_t total_us)
{
	return queue_us > 0 &&
	       fraction_to_ratio_e6((uint64_t)queue_us, (uint64_t)total_us) >= QUEUE_SHARE_RATIO_E6;
}

/* What a direction delivers of its shaper rate; zero without a valid measurement. */
static uint64_t delivery_ratio_e6(
	const struct controller_direction *direction,
	const struct controller_direction_input *input
)
{
	if (!input->valid)
		return 0U;
	return load_ratio_e6(
		input->traffic_rate_bits_per_second,
		direction->shaper_rate_bits_per_second
	);
}

void controller_update(
	struct controller *controller,
	const struct controller_input *input,
	struct controller_output *output
)
{
	const struct controller_config *config = &controller->config;
	uint64_t download_delivery_e6 = delivery_ratio_e6(&controller->download, &input->download);
	const struct controller_queue_input *queue = &input->queue;
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
		.ceiling_bits_per_second = download_ceiling(controller, input),
		.timestamp_us = input->timestamp_us,
	};
	struct direction_update upload = {
		.input = &input->upload,
		.latency = input->upload_latency,
		.bufferbloat_attributed = !config->shared_delay ||
					  download_delivery_e6 <=
						  config->high_load_threshold_ratio_e6 ||
					  download_delivery_e6 >= FULL_DELIVERY_RATIO_E6,
		.ceiling_bits_per_second = UINT64_MAX,
		.timestamp_us = input->timestamp_us,
	};

	/*
	 * Measured per-direction queues replace the heuristic, unless they are too
	 * small to explain the shared delay, which then arose outside the paths
	 * TCP observes.
	 */
	if (config->shared_delay && queue->valid &&
	    queue->download_us + queue->upload_us >= QUEUE_ATTRIBUTION_MINIMUM_US) {
		int64_t total = queue->download_us + queue->upload_us;

		download.bufferbloat_attributed = holds_queue_share(queue->download_us, total);
		upload.bufferbloat_attributed = holds_queue_share(queue->upload_us, total);
		/*
		 * Split the round-trip delta by the measured shares instead of RTT/2
		 * each way, so a one-sided queue counts at its full size.
		 */
		if (download.latency.valid && upload.latency.valid) {
			int64_t round_trip =
				download.latency.owd_delta_us + upload.latency.owd_delta_us;

			download.latency.owd_delta_us =
				signed_rounded_divide(round_trip * queue->download_us, total);
			upload.latency.owd_delta_us = round_trip - download.latency.owd_delta_us;
		}
	}

	update_direction(&controller->download, controller, &download, &output->download);
	update_direction(&controller->upload, controller, &upload, &output->upload);
}

static void compensate_direction(
	struct controller_direction *direction,
	const struct controller_direction_config *configured,
	uint64_t wire_packet_bits
)
{
	struct controller_direction_config *effective = &direction->config;
	uint64_t compensation =
		serialization_us(wire_packet_bits, direction->shaper_rate_bits_per_second);

	effective->average_delay_maximum_adjust_up_us =
		saturating_add(configured->average_delay_maximum_adjust_up_us, compensation);
	effective->delay_threshold_us =
		saturating_add(configured->delay_threshold_us, compensation);
	effective->average_delay_maximum_adjust_down_us =
		saturating_add(configured->average_delay_maximum_adjust_down_us, compensation);
}

void controller_set_serialization_compensation(
	struct controller *controller,
	uint64_t download_wire_packet_bits,
	uint64_t upload_wire_packet_bits
)
{
	compensate_direction(
		&controller->download,
		&controller->config.download,
		download_wire_packet_bits
	);
	compensate_direction(
		&controller->upload,
		&controller->config.upload,
		upload_wire_packet_bits
	);
}

static uint64_t input_load_ratio_e6(const struct controller_direction_input *input)
{
	return load_ratio_e6(input->traffic_rate_bits_per_second, input->cake_rate_bits_per_second);
}

enum controller_load controller_load(
	const struct controller *controller,
	const struct controller_direction_input *input,
	uint64_t active_threshold_bits_per_second
)
{
	const struct controller_config *config = &controller->config;

	if (input_load_ratio_e6(input) > config->high_load_threshold_ratio_e6)
		return CONTROLLER_LOAD_HIGH;
	if (input->traffic_rate_bits_per_second > active_threshold_bits_per_second)
		return CONTROLLER_LOAD_LOW;
	return CONTROLLER_LOAD_IDLE;
}

bool controller_low_load(
	const struct controller *controller,
	const struct controller_direction_input *download,
	const struct controller_direction_input *upload
)
{
	const struct controller_config *config = &controller->config;
	uint64_t threshold_e6 = config->high_load_threshold_ratio_e6;

	return input_load_ratio_e6(download) < threshold_e6 &&
	       input_load_ratio_e6(upload) < threshold_e6;
}

void controller_set_minimum_rates(struct controller *controller, uint64_t timestamp_us)
{
	struct controller_direction *directions[] = { &controller->download, &controller->upload };
	size_t index;

	for (index = 0U; index < ARRAY_SIZE(directions); index++) {
		struct controller_direction *direction = directions[index];

		if (!direction->config.adjust)
			continue;
		direction->shaper_rate_bits_per_second =
			direction->config.minimum_rate_bits_per_second;
		direction->last_congestion_adjustment_us = timestamp_us;
		direction->last_decay_adjustment_us = timestamp_us;
		direction->initial_rate_pending = false;
	}
}

static bool
rate_above(const struct controller_direction_input *input, uint64_t threshold_bits_per_second)
{
	return input->valid && input->traffic_rate_bits_per_second > threshold_bits_per_second;
}

void activity_init(
	struct controller_activity *activity,
	const struct controller_activity_config *config
)
{
	*activity = (struct controller_activity){
		.config = *config,
		.state = CONTROLLER_RUNNING,
	};
}

void activity_update(
	struct controller_activity *activity,
	const struct controller_activity_input *input,
	struct controller_activity_output *output
)
{
	const struct controller_activity_config *config = &activity->config;
	enum controller_activity_state previous = activity->state;
	uint64_t response_age = saturating_sub(input->timestamp_us, input->last_response_us);
	bool check_global_timeout = false;

	memset(output, 0, sizeof(*output));
	if (response_age < config->global_timeout_us)
		activity->global_timeout_reported = false;
	switch (activity->state) {
	case CONTROLLER_RUNNING:
		if (input->timestamp_us < input->grace_until_us)
			return;
		if (response_age > config->stall_timeout_us) {
			activity->idle_started_us = 0U;
			output->check_stall_loads = true;
			check_global_timeout = true;
			if (!rate_above(&input->download, config->stall_threshold_bits_per_second) ||
			    !rate_above(&input->upload, config->stall_threshold_bits_per_second))
				activity->state = CONTROLLER_STALL;
		} else if (config->enable_sleep && input->download.valid && input->upload.valid &&
			   !rate_above(&input->download, config->active_threshold_bits_per_second) &&
			   !rate_above(&input->upload, config->active_threshold_bits_per_second)) {
			/* Only healthy probes and valid counters can establish idle time. */
			if (activity->idle_started_us == 0U) {
				activity->idle_started_us = input->timestamp_us;
			} else if (input->timestamp_us - activity->idle_started_us >
				   config->sustained_idle_us) {
				activity->state = CONTROLLER_IDLE;
				activity->idle_started_us = 0U;
			}
		} else {
			activity->idle_started_us = 0U;
		}
		break;
	case CONTROLLER_IDLE:
		if (rate_above(&input->download, config->active_threshold_bits_per_second) ||
		    rate_above(&input->upload, config->active_threshold_bits_per_second))
			activity->state = CONTROLLER_RUNNING;
		break;
	case CONTROLLER_STALL:
		if (response_age <= config->stall_timeout_us ||
		    (rate_above(&input->download, config->stall_threshold_bits_per_second) &&
		     rate_above(&input->upload, config->stall_threshold_bits_per_second))) {
			activity->state = CONTROLLER_RUNNING;
		}
		check_global_timeout = true;
		break;
	}
	if (check_global_timeout && response_age >= config->global_timeout_us) {
		output->global_timeout_started = !activity->global_timeout_reported;
		activity->global_timeout_reported = true;
		output->restart_pingers = input->timestamp_us >= input->last_pinger_start_us &&
					  input->timestamp_us - input->last_pinger_start_us >=
						  config->global_timeout_us;
	}
	output->state_changed = previous != activity->state;
}
