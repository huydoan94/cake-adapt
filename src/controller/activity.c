#include "controller/activity.h"
#include "controller/controller.h"
#include "common/arith.h"
#include "common/rate.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint64_t input_load_ratio_e6(const struct controller_direction_input *input)
{
	return load_ratio_e6(input->traffic_rate_bps, input->cake_rate_bps);
}

enum controller_load controller_load(
	const struct controller *controller,
	const struct controller_direction_input *input,
	uint64_t active_threshold_bps
)
{
	const struct controller_config *config = &controller->config;

	if (input_load_ratio_e6(input) > config->high_load_threshold_ratio_e6)
		return CONTROLLER_LOAD_HIGH;
	if (input->traffic_rate_bps > active_threshold_bps)
		return CONTROLLER_LOAD_LOW;
	return CONTROLLER_LOAD_IDLE;
}

bool controller_low_load(
	const struct controller *controller,
	const struct controller_direction_input inputs[DIRECTION_COUNT]
)
{
	const struct controller_config *config = &controller->config;

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++)
		if (input_load_ratio_e6(&inputs[direction]) >= config->high_load_threshold_ratio_e6)
			return false;
	return true;
}

static bool rate_above(const struct controller_direction_input *input, uint64_t threshold_bps)
{
	return input->valid && input->traffic_rate_bps > threshold_bps;
}

/* How many directions carry traffic above the threshold. */
static size_t count_above(const struct controller_activity_input *input, uint64_t threshold_bps)
{
	size_t count = 0U;

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++)
		if (rate_above(&input->directions[direction], threshold_bps))
			count++;
	return count;
}

/* Some traffic above the threshold: enough to wake from idle. */
static bool either_above(const struct controller_activity_input *input, uint64_t threshold_bps)
{
	return count_above(input, threshold_bps) > 0U;
}

/* Traffic above the threshold both ways: a link that still works, not a stall. */
static bool both_above(const struct controller_activity_input *input, uint64_t threshold_bps)
{
	return count_above(input, threshold_bps) == DIRECTION_COUNT;
}

/* Both counters read, so a quiet link is known to be quiet. */
static bool both_valid(const struct controller_activity_input *input)
{
	for (size_t index = 0U; index < DIRECTION_COUNT; index++) {
		const struct controller_direction_input *direction = &input->directions[index];

		if (!direction->valid)
			return false;
	}
	return true;
}

void controller_activity_init(
	struct controller_activity *activity,
	const struct controller_activity_config *config
)
{
	*activity = (struct controller_activity){
		.config = *config,
		.state = CONTROLLER_ACTIVITY_RUNNING,
	};
}

void controller_activity_update(
	struct controller_activity *activity,
	const struct controller_activity_input *input,
	struct controller_activity_output *output
)
{
	const struct controller_activity_config *config = &activity->config;
	enum controller_activity_state previous = activity->state;
	uint64_t response_age_us = saturating_sub(input->timestamp_us, input->last_response_us);
	bool check_global_timeout = false;

	memset(output, 0, sizeof(*output));
	if (response_age_us < config->global_timeout_us)
		activity->global_timeout_reported = false;
	switch (activity->state) {
	case CONTROLLER_ACTIVITY_RUNNING:
		if (input->timestamp_us < input->grace_until_us)
			return;
		if (response_age_us > config->stall_timeout_us) {
			activity->idle_started_us = 0U;
			output->check_stall_loads = true;
			check_global_timeout = true;
			if (!both_above(input, config->stall_threshold_bps))
				activity->state = CONTROLLER_ACTIVITY_STALL;
		} else if (config->enable_sleep && both_valid(input) &&
			   !either_above(input, config->active_threshold_bps)) {
			/* Only healthy probes and valid counters can establish idle time. */
			if (activity->idle_started_us == 0U) {
				activity->idle_started_us = input->timestamp_us;
			} else if (input->timestamp_us - activity->idle_started_us >
				   config->sustained_idle_us) {
				activity->state = CONTROLLER_ACTIVITY_IDLE;
				activity->idle_started_us = 0U;
			}
		} else {
			activity->idle_started_us = 0U;
		}
		break;
	case CONTROLLER_ACTIVITY_IDLE:
		if (either_above(input, config->active_threshold_bps))
			activity->state = CONTROLLER_ACTIVITY_RUNNING;
		break;
	case CONTROLLER_ACTIVITY_STALL:
		if (response_age_us <= config->stall_timeout_us ||
		    both_above(input, config->stall_threshold_bps))
			activity->state = CONTROLLER_ACTIVITY_RUNNING;
		check_global_timeout = true;
		break;
	}
	if (check_global_timeout && response_age_us >= config->global_timeout_us) {
		output->global_timeout_started = !activity->global_timeout_reported;
		activity->global_timeout_reported = true;
		output->restart_pingers = input->timestamp_us >= input->last_pinger_start_us &&
					  input->timestamp_us - input->last_pinger_start_us >=
						  config->global_timeout_us;
	}
	output->state_changed = previous != activity->state;
}
