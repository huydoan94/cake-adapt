#include "controller/direction.h"
#include "common/arith.h"
#include "common/clock.h"
#include "common/rate.h"
#include "constants/cake.h"
#include "constants/units.h"

#include <stdbool.h>
#include <stdint.h>

uint64_t rate_holdable(uint64_t rate_bps)
{
	return rate_bps / SHAPER_RATE_STEP_BPS * SHAPER_RATE_STEP_BPS;
}

/* rate * factor, down to a rate CAKE can hold. */
static uint64_t scale_rate(uint64_t rate_bps, uint64_t factor_e6)
{
	return rate_holdable(ratio_of(rate_bps, factor_e6));
}

static uint64_t clamp_rate(uint64_t rate_bps, const struct controller_direction_config *config)
{
	if (rate_bps < config->minimum_rate_bps)
		return config->minimum_rate_bps;
	if (rate_bps > config->maximum_rate_bps)
		return config->maximum_rate_bps;
	return rate_bps;
}

static uint64_t
rate_toward_base(uint64_t rate_bps, uint64_t base_rate_bps, const struct controller_config *config)
{
	if (rate_bps > base_rate_bps)
		return max_u64(
			scale_rate(rate_bps, config->rate_adjust_down_load_low_ratio_e6),
			base_rate_bps
		);
	if (rate_bps < base_rate_bps)
		return min_u64(
			scale_rate(rate_bps, config->rate_adjust_up_load_low_ratio_e6),
			base_rate_bps
		);
	return rate_bps;
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
static uint64_t delay_adjustment_e6(int64_t delay_us, uint64_t from_us, uint64_t to_us)
{
	if (to_us <= from_us || (delay_us > 0 && (uint64_t)delay_us >= to_us))
		return RATIO_ONE_E6;
	if (delay_us <= 0 || (uint64_t)delay_us <= from_us)
		return 0U;
	return fraction_to_ratio_e6((uint64_t)delay_us - from_us, to_us - from_us);
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
			thresholds->owd_delta_delay_threshold_us,
			thresholds->average_owd_delta_maximum_adjust_down_us
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
	uint64_t threshold_us = thresholds->owd_delta_delay_threshold_us;
	uint64_t full_us = thresholds->average_owd_delta_maximum_adjust_up_us;
	int64_t delay_us = direction->average_delay_us;
	uint64_t adjustment_e6;

	if (threshold_us <= full_us || delay_us <= 0 || (uint64_t)delay_us <= full_us)
		adjustment_e6 = RATIO_ONE_E6;
	else if ((uint64_t)delay_us < threshold_us)
		adjustment_e6 = fraction_to_ratio_e6(
			threshold_us - (uint64_t)delay_us,
			threshold_us - full_us
		);
	else
		adjustment_e6 = 0U;
	return interpolate_factor_e6(
		config->rate_minimum_adjust_up_load_high_ratio_e6,
		config->rate_maximum_adjust_up_load_high_ratio_e6,
		adjustment_e6
	);
}

enum controller_rate_reason rate_adjust(
	struct controller_direction *direction,
	const struct controller *controller,
	const struct direction_update *update
)
{
	const struct controller_config *config = &controller->config;
	const struct controller_direction_config *limits = &direction->config;
	const struct controller_direction_input *input = update->input;
	const struct controller_latency_input *latency = &update->latency;
	uint64_t timestamp_us = update->timestamp_us;
	uint64_t previous_rate_bps = direction->shaper_rate_bps;
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
	if (!latency->valid)
		return CONTROLLER_RATE_UNCHANGED;

	high_load = load_ratio_e6(input->traffic_rate_bps, previous_rate_bps) >
		    config->high_load_threshold_ratio_e6;
	/* Both a cut and an increase wait out the bufferbloat refractory period. */
	refractory_elapsed = interval_elapsed(
		timestamp_us,
		direction->last_congestion_adjustment_us,
		config->bufferbloat_refractory_period_us
	);
	if (congested && update->bufferbloat_attributed && refractory_elapsed) {
		direction->shaper_rate_bps =
			scale_rate(previous_rate_bps, downward_factor_e6(direction, config));
		direction->last_congestion_adjustment_us = timestamp_us;
		/* Do not let low-load decay immediately undo a congestion cut. */
		direction->last_decay_adjustment_us = timestamp_us;
	} else if (!congested && high_load &&
		   input->traffic_sample_id != direction->last_increase_sample_id &&
		   refractory_elapsed) {
		/* Like upstream achieved_rate_updated: one increase per load sample,
		 * even when the factor is one or the maximum rate clips the result. */
		direction->last_increase_sample_id = input->traffic_sample_id;
		direction->shaper_rate_bps =
			scale_rate(previous_rate_bps, upward_factor_e6(direction, config));
		/* Give the increased rate a full decay interval to be observed. */
		direction->last_decay_adjustment_us = timestamp_us;
	} else if (!congested && !high_load && previous_rate_bps != limits->base_rate_bps &&
		   interval_elapsed(
			   timestamp_us,
			   direction->last_decay_adjustment_us,
			   config->decay_refractory_period_us
		   )) {
		/* With low load, converge by 1% steps instead of jumping to base. */
		direction->shaper_rate_bps =
			rate_toward_base(previous_rate_bps, limits->base_rate_bps, config);
		direction->last_decay_adjustment_us = timestamp_us;
	}

	if (direction->shaper_rate_bps > update->ceiling_bps) {
		direction->shaper_rate_bps = update->ceiling_bps;
		capped = true;
	}
	/* The minimum rate still wins over the ceiling. */
	direction->shaper_rate_bps = clamp_rate(direction->shaper_rate_bps, limits);
	if (direction->shaper_rate_bps == previous_rate_bps)
		return CONTROLLER_RATE_UNCHANGED;
	if (capped)
		return CONTROLLER_RATE_ACK_SHARE;
	if (congested)
		return CONTROLLER_RATE_CONGESTION;
	if (high_load)
		return CONTROLLER_RATE_HIGH_LOAD;
	return CONTROLLER_RATE_RETURN_TO_BASE;
}

void rate_set_output(
	const struct controller_direction *direction,
	const struct controller_direction_input *input,
	enum controller_rate_reason reason,
	struct controller_direction_output *output
)
{
	const struct controller_direction_config *limits = &direction->config;

	if (!limits->adjust) {
		output->rate_bps = input->cake_rate_bps;
		output->rate_changed = false;
		output->rate_reason = CONTROLLER_RATE_UNCHANGED;
		return;
	}

	output->rate_bps = direction->shaper_rate_bps;
	/* Like cake-autorate's first set_shaper_rates(), the base rate is written once
	 * even when CAKE already holds it. */
	output->rate_changed = input->valid && (reason == CONTROLLER_RATE_INITIAL ||
						input->cake_rate_bps != output->rate_bps);
	output->rate_reason = output->rate_changed && reason == CONTROLLER_RATE_UNCHANGED ?
				      CONTROLLER_RATE_RECONCILE :
				      reason;
}
