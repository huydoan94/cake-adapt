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

static uint64_t scale_rate(uint64_t rate_bits_per_second, uint64_t factor, uint64_t factor_scale)
{
	/* cake-autorate performs shaper calculations in whole kbit/s. */
	uint64_t rate_kilobits_per_second = rate_bits_per_second / KILOBIT;
	uint64_t scaled_kilobits_per_second = saturating_add(
		saturating_mul(rate_kilobits_per_second / factor_scale, factor),
		saturating_mul(rate_kilobits_per_second % factor_scale, factor) / factor_scale
	);

	return saturating_mul(scaled_kilobits_per_second, KILOBIT);
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
	if (rate > base_rate) {
		return max_u64(
			scale_rate(rate, config->rate_adjust_down_low_load_per_thousand, THOUSAND),
			base_rate
		);
	}
	if (rate < base_rate) {
		return min_u64(
			scale_rate(rate, config->rate_adjust_up_low_load_per_thousand, THOUSAND),
			base_rate
		);
	}
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

int controller_init(struct controller *controller, const struct controller_config *config)
{
	memset(controller, 0, sizeof(*controller));
	if (config->bufferbloat_detection_window == 0U ||
	    config->bufferbloat_detection_threshold > config->bufferbloat_detection_window ||
	    config->rate_minimum_adjust_down_bufferbloat_per_thousand > UINT64_MAX / THOUSAND ||
	    config->rate_maximum_adjust_down_bufferbloat_per_thousand > UINT64_MAX / THOUSAND ||
	    config->rate_minimum_adjust_up_high_load_per_thousand > UINT64_MAX / THOUSAND ||
	    config->rate_maximum_adjust_up_high_load_per_thousand > UINT64_MAX / THOUSAND ||
	    config->rate_adjust_down_low_load_per_thousand > UINT64_MAX / THOUSAND ||
	    config->rate_adjust_up_low_load_per_thousand > UINT64_MAX / THOUSAND) {
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
		uint64_t recovery_threshold =
			percentage_of(input->cake_rate_bits_per_second, SATURATION_EXIT_PERCENT);

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
	    percentage_of(input->cake_rate_bits_per_second, SATURATION_ENTER_PERCENT)) {
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
		direction->average_delay_microseconds = 0;
		return direction->congestion;
	}

	index = direction->delay_next_sample;
	sample = &direction->delay_samples[index];
	delayed = &direction->delayed_samples[index];

	/* Maintain a fixed rolling window without rescanning every sample. */
	direction->delay_sum_microseconds -= *sample;
	direction->delay_sum_microseconds += latency->owd_delta_microseconds;

	/* Preserve the classification made when this sample entered the window. */
	if (*delayed != 0U)
		direction->delayed_sample_count--;
	if (latency->owd_delta_microseconds > 0 &&
	    (uint64_t)latency->owd_delta_microseconds >
		    direction->config.delay_threshold_microseconds) {
		direction->delayed_sample_count++;
		*delayed = 1U;
	} else {
		*delayed = 0U;
	}
	*sample = latency->owd_delta_microseconds;

	direction->delay_next_sample = index + 1U;
	if (direction->delay_next_sample == config->bufferbloat_detection_window)
		direction->delay_next_sample = 0U;
	direction->average_delay_microseconds =
		direction->delay_sum_microseconds / (int64_t)config->bufferbloat_detection_window;
	direction->congestion = direction->delayed_sample_count >=
						config->bufferbloat_detection_threshold ?
					CONTROLLER_CONGESTION_DETECTED :
					CONTROLLER_CONGESTION_CLEAR;
	return direction->congestion;
}

static uint64_t interpolate_factor(
	uint64_t minimum_factor_per_thousand,
	uint64_t maximum_factor_per_thousand,
	uint64_t adjustment
)
{
	uint64_t difference;
	uint64_t base = minimum_factor_per_thousand * THOUSAND;

	/*
	 * cake-autorate keeps the configured endpoints per-thousand, then keeps
	 * three additional decimal places while interpolating between them.
	 */
	if (minimum_factor_per_thousand >= maximum_factor_per_thousand) {
		difference = minimum_factor_per_thousand - maximum_factor_per_thousand;
		return base - adjustment * difference;
	}

	difference = maximum_factor_per_thousand - minimum_factor_per_thousand;
	return base + adjustment * difference;
}

static uint64_t
downward_factor(const struct controller_direction *direction, const struct controller_config *config)
{
	const struct controller_direction_config *thresholds = &direction->config;
	int64_t average_delay_microseconds = direction->average_delay_microseconds;
	uint64_t adjustment;
	uint64_t delay_above_threshold;
	uint64_t adjustment_range;

	if (thresholds->average_delay_maximum_adjust_down_microseconds <=
	    thresholds->delay_threshold_microseconds) {
		adjustment = THOUSAND;
	} else if (average_delay_microseconds > 0 &&
		   (uint64_t)average_delay_microseconds >
			   thresholds->delay_threshold_microseconds) {
		adjustment_range = thresholds->average_delay_maximum_adjust_down_microseconds -
				   thresholds->delay_threshold_microseconds;
		if ((uint64_t)average_delay_microseconds >=
		    thresholds->average_delay_maximum_adjust_down_microseconds) {
			adjustment = THOUSAND;
		} else {
			delay_above_threshold = (uint64_t)average_delay_microseconds -
						thresholds->delay_threshold_microseconds;
			adjustment = THOUSAND * delay_above_threshold / adjustment_range;
		}
	} else {
		adjustment = 0U;
	}

	return interpolate_factor(
		config->rate_minimum_adjust_down_bufferbloat_per_thousand,
		config->rate_maximum_adjust_down_bufferbloat_per_thousand,
		adjustment
	);
}

static uint64_t
upward_factor(const struct controller_direction *direction, const struct controller_config *config)
{
	const struct controller_direction_config *thresholds = &direction->config;
	int64_t average_delay_microseconds = direction->average_delay_microseconds;
	uint64_t adjustment;
	uint64_t delay_below_threshold;
	uint64_t adjustment_range;

	if (thresholds->delay_threshold_microseconds <=
		    thresholds->average_delay_maximum_adjust_up_microseconds ||
	    average_delay_microseconds <= 0 ||
	    (uint64_t)average_delay_microseconds <=
		    thresholds->average_delay_maximum_adjust_up_microseconds) {
		adjustment = THOUSAND;
	} else if ((uint64_t)average_delay_microseconds <
		   thresholds->delay_threshold_microseconds) {
		delay_below_threshold = thresholds->delay_threshold_microseconds -
					(uint64_t)average_delay_microseconds;
		adjustment_range = thresholds->delay_threshold_microseconds -
				   thresholds->average_delay_maximum_adjust_up_microseconds;
		adjustment = THOUSAND * delay_below_threshold / adjustment_range;
	} else {
		adjustment = 0U;
	}

	return interpolate_factor(
		config->rate_minimum_adjust_up_high_load_per_thousand,
		config->rate_maximum_adjust_up_high_load_per_thousand,
		adjustment
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
	uint64_t timestamp_microseconds;
};

static enum controller_rate_reason adjust_rate(
	struct controller_direction *direction,
	const struct controller_config *config,
	const struct direction_update *update
)
{
	const struct controller_direction_input *input = update->input;
	uint64_t timestamp_microseconds = update->timestamp_microseconds;
	uint64_t previous_rate = direction->shaper_rate_bits_per_second;
	bool congested = direction->congestion == CONTROLLER_CONGESTION_DETECTED;
	bool refractory_elapsed;
	bool high_load;
	bool capped = false;

	if (!direction->config.adjust || !input->valid)
		return CONTROLLER_RATE_UNCHANGED;
	if (direction->initial_rate_pending) {
		direction->initial_rate_pending = false;
		direction->last_congestion_adjustment_microseconds = timestamp_microseconds;
		direction->last_decay_adjustment_microseconds = timestamp_microseconds;
		return CONTROLLER_RATE_INITIAL;
	}
	if (!update->latency.valid)
		return CONTROLLER_RATE_UNCHANGED;

	high_load = load_percent(input->traffic_rate_bits_per_second, previous_rate) >
		    config->high_load_threshold_percent;
	/* Both a cut and an increase wait out the bufferbloat refractory period. */
	refractory_elapsed = interval_elapsed(
		timestamp_microseconds,
		direction->last_congestion_adjustment_microseconds,
		config->bufferbloat_refractory_period_microseconds
	);
	if (congested && update->bufferbloat_attributed && refractory_elapsed) {
		direction->shaper_rate_bits_per_second =
			scale_rate(previous_rate, downward_factor(direction, config), MILLION);
		direction->last_congestion_adjustment_microseconds = timestamp_microseconds;
		/* Do not let low-load decay immediately undo a congestion cut. */
		direction->last_decay_adjustment_microseconds = timestamp_microseconds;
	} else if (!congested && high_load &&
		   input->traffic_sample_id != direction->last_increase_sample_id &&
		   refractory_elapsed) {
		/* Like upstream achieved_rate_updated: one increase per load sample,
		 * even when the factor is one or the maximum rate clips the result. */
		direction->last_increase_sample_id = input->traffic_sample_id;
		direction->shaper_rate_bits_per_second =
			scale_rate(previous_rate, upward_factor(direction, config), MILLION);
		/* Give the increased rate a full decay interval to be observed. */
		direction->last_decay_adjustment_microseconds = timestamp_microseconds;
	} else if (!congested && !high_load &&
		   previous_rate != direction->config.base_rate_bits_per_second &&
		   interval_elapsed(
			   timestamp_microseconds,
			   direction->last_decay_adjustment_microseconds,
			   config->decay_refractory_period_microseconds
		   )) {
		/* With low load, converge by 1% steps instead of jumping to base. */
		direction->shaper_rate_bits_per_second = rate_toward_base(
			previous_rate,
			direction->config.base_rate_bits_per_second,
			config
		);
		direction->last_decay_adjustment_microseconds = timestamp_microseconds;
	}

	if (direction->shaper_rate_bits_per_second > update->ceiling_bits_per_second) {
		direction->shaper_rate_bits_per_second = update->ceiling_bits_per_second;
		capped = true;
	}
	/* The minimum rate still wins over the ceiling. */
	direction->shaper_rate_bits_per_second =
		clamp_rate(direction->shaper_rate_bits_per_second, &direction->config);
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
	const struct controller_config *config,
	const struct direction_update *update,
	struct controller_direction_output *output
)
{
	enum controller_line_state previous_state = direction->state;
	enum controller_congestion_state previous_congestion = direction->congestion;
	bool attributed = update->bufferbloat_attributed;

	output->state = update_line_state(direction, update->input);
	output->congestion = update_congestion(direction, config, &update->latency);
	output->average_delay_microseconds = direction->average_delay_microseconds;
	set_rate_output(direction, update->input, adjust_rate(direction, config, update), output);

	output->delayed_sample_count = direction->delayed_sample_count;
	output->state_changed = output->state != previous_state;
	output->congestion_changed = output->congestion != previous_congestion;
	output->bufferbloat_attributed = attributed;
	output->bufferbloat_attribution_changed = attributed != direction->bufferbloat_attributed;
	direction->bufferbloat_attributed = attributed;
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
	uint64_t room = controller->upload.shaper_rate_bits_per_second;
	uint64_t ack_rate = input->acks.upload_ack_rate_bits_per_second;
	uint64_t minimum = mul_div(room, controller->config.ul_congest_ack_share_percent, PERCENT);
	uint64_t other;
	uint64_t taken;
	uint64_t allowed;

	if (controller->config.ul_congest_ack_share_percent == 0U || !input->acks.valid ||
	    ack_rate == 0U || !input->upload.valid || !input->download.valid ||
	    load_percent(input->upload.traffic_rate_bits_per_second, room) <=
		    controller->config.high_load_threshold_percent) {
		return UINT64_MAX;
	}
	other = saturating_sub(input->acks.upload_rate_bits_per_second, ack_rate);
	taken = other + mul_div(room, UPLOAD_ACK_HEADROOM_PERCENT, PERCENT);
	allowed = saturating_sub(room, taken);
	if (allowed < minimum)
		allowed = minimum;
	if (ack_rate <= allowed)
		return UINT64_MAX;
	/* Whole kbit/s like other rates. */
	return mul_div(input->download.traffic_rate_bits_per_second, allowed, ack_rate) / KILOBIT *
	       KILOBIT;
}

void controller_update(
	struct controller *controller,
	const struct controller_input *input,
	struct controller_output *output
)
{
	unsigned int download_delivery =
		input->download.valid ? load_percent(
						input->download.traffic_rate_bits_per_second,
						controller->download.shaper_rate_bits_per_second
					) :
					0U;
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
		.bufferbloat_attributed = !controller->config.shared_delay ||
					  download_delivery < FULL_DELIVERY_PERCENT,
		.ceiling_bits_per_second = download_ceiling(controller, input),
		.timestamp_microseconds = input->timestamp_microseconds,
	};
	struct direction_update upload = {
		.input = &input->upload,
		.latency = input->upload_latency,
		.bufferbloat_attributed = !controller->config.shared_delay ||
					  download_delivery <=
						  controller->config.high_load_threshold_percent ||
					  download_delivery >= FULL_DELIVERY_PERCENT,
		.ceiling_bits_per_second = UINT64_MAX,
		.timestamp_microseconds = input->timestamp_microseconds,
	};

	/*
	 * Measured per-direction queues replace the heuristic, unless they are too
	 * small to explain the shared delay, which then arose outside the paths
	 * TCP observes.
	 */
	if (controller->config.shared_delay && queue->valid &&
	    queue->download_microseconds + queue->upload_microseconds >=
		    QUEUE_ATTRIBUTION_MINIMUM_MICROSECONDS) {
		int64_t total = queue->download_microseconds + queue->upload_microseconds;

		download.bufferbloat_attributed =
			queue->download_microseconds * QUEUE_SHARE_DIVISOR >= total;
		upload.bufferbloat_attributed = queue->upload_microseconds * QUEUE_SHARE_DIVISOR >=
						total;
		/*
		 * Split the round-trip delta by the measured shares instead of RTT/2
		 * each way, so a one-sided queue counts at its full size.
		 */
		if (download.latency.valid && upload.latency.valid) {
			int64_t round_trip = download.latency.owd_delta_microseconds +
					     upload.latency.owd_delta_microseconds;

			download.latency.owd_delta_microseconds =
				round_trip * queue->download_microseconds / total;
			upload.latency.owd_delta_microseconds =
				round_trip - download.latency.owd_delta_microseconds;
		}
	}

	update_direction(&controller->download, &controller->config, &download, &output->download);
	update_direction(&controller->upload, &controller->config, &upload, &output->upload);
}

static void compensate_direction(
	struct controller_direction *direction,
	const struct controller_direction_config *configured,
	uint64_t wire_packet_bits
)
{
	uint64_t compensation =
		serialization_microseconds(wire_packet_bits, direction->shaper_rate_bits_per_second);

	direction->config.average_delay_maximum_adjust_up_microseconds = saturating_add(
		configured->average_delay_maximum_adjust_up_microseconds,
		compensation
	);
	direction->config.delay_threshold_microseconds =
		saturating_add(configured->delay_threshold_microseconds, compensation);
	direction->config.average_delay_maximum_adjust_down_microseconds = saturating_add(
		configured->average_delay_maximum_adjust_down_microseconds,
		compensation
	);
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

static unsigned int input_load_percent(const struct controller_direction_input *input)
{
	return load_percent(input->traffic_rate_bits_per_second, input->cake_rate_bits_per_second);
}

enum controller_load controller_load(
	const struct controller *controller,
	const struct controller_direction_input *input,
	uint64_t active_threshold_bits_per_second
)
{
	if (input_load_percent(input) > controller->config.high_load_threshold_percent)
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
	uint64_t threshold = controller->config.high_load_threshold_percent;

	return input_load_percent(download) < threshold && input_load_percent(upload) < threshold;
}

void controller_set_minimum_rates(struct controller *controller, uint64_t timestamp_microseconds)
{
	struct controller_direction *directions[] = { &controller->download, &controller->upload };
	size_t index;

	for (index = 0U; index < ARRAY_SIZE(directions); index++) {
		struct controller_direction *direction = directions[index];

		if (!direction->config.adjust)
			continue;
		direction->shaper_rate_bits_per_second =
			direction->config.minimum_rate_bits_per_second;
		direction->last_congestion_adjustment_microseconds = timestamp_microseconds;
		direction->last_decay_adjustment_microseconds = timestamp_microseconds;
		direction->initial_rate_pending = false;
	}
}

/* Compared in whole kbit/s, like cake-autorate. */
static bool rate_above(const struct controller_direction_input *input, uint64_t threshold)
{
	return input->valid && input->traffic_rate_bits_per_second / KILOBIT > threshold / KILOBIT;
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
	uint64_t response_age =
		saturating_sub(input->timestamp_microseconds, input->last_response_microseconds);
	bool check_global_timeout = false;

	memset(output, 0, sizeof(*output));
	if (response_age < config->global_timeout_microseconds)
		activity->global_timeout_reported = false;
	switch (activity->state) {
	case CONTROLLER_RUNNING:
		if (input->timestamp_microseconds < input->grace_until_microseconds)
			return;
		if (response_age > config->stall_timeout_microseconds) {
			activity->idle_started_microseconds = 0U;
			output->check_stall_loads = true;
			check_global_timeout = true;
			if (!rate_above(&input->download, config->stall_threshold_bits_per_second) ||
			    !rate_above(&input->upload, config->stall_threshold_bits_per_second))
				activity->state = CONTROLLER_STALL;
		} else if (config->enable_sleep && input->download.valid && input->upload.valid &&
			   !rate_above(&input->download, config->active_threshold_bits_per_second) &&
			   !rate_above(&input->upload, config->active_threshold_bits_per_second)) {
			/* Only healthy probes and valid counters can establish idle time. */
			if (activity->idle_started_microseconds == 0U) {
				activity->idle_started_microseconds = input->timestamp_microseconds;
			} else if (input->timestamp_microseconds -
					   activity->idle_started_microseconds >
				   config->sustained_idle_microseconds) {
				activity->state = CONTROLLER_IDLE;
				activity->idle_started_microseconds = 0U;
			}
		} else {
			activity->idle_started_microseconds = 0U;
		}
		break;
	case CONTROLLER_IDLE:
		if (rate_above(&input->download, config->active_threshold_bits_per_second) ||
		    rate_above(&input->upload, config->active_threshold_bits_per_second))
			activity->state = CONTROLLER_RUNNING;
		break;
	case CONTROLLER_STALL:
		if (response_age <= config->stall_timeout_microseconds ||
		    (rate_above(&input->download, config->stall_threshold_bits_per_second) &&
		     rate_above(&input->upload, config->stall_threshold_bits_per_second))) {
			activity->state = CONTROLLER_RUNNING;
		}
		check_global_timeout = true;
		break;
	}
	if (check_global_timeout && response_age >= config->global_timeout_microseconds) {
		output->global_timeout_started = !activity->global_timeout_reported;
		activity->global_timeout_reported = true;
		output->restart_pingers =
			input->timestamp_microseconds >= input->last_pinger_start_microseconds &&
			input->timestamp_microseconds - input->last_pinger_start_microseconds >=
				config->global_timeout_microseconds;
	}
	output->state_changed = previous != activity->state;
}
