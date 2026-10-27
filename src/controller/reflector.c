#include "controller/reflector.h"
#include "common/arith.h"
#include "common/clock.h"

#include <stdlib.h>
#include <string.h>

int reflector_health_init(
	struct reflector_health *health,
	const struct reflector_health_config *config,
	uint64_t start_us
)
{
	health->offences = calloc(config->detection_window, sizeof(*health->offences));
	if (health->offences == NULL)
		return -1;
	health->config = config;
	health->last_response_us = start_us;
	health->offence_index = 0U;
	health->offence_count = 0U;
	return 0;
}

void reflector_health_cleanup(struct reflector_health *health)
{
	free(health->offences);
	health->offences = NULL;
	health->offence_index = 0U;
	health->offence_count = 0U;
}

void reflector_health_reset(struct reflector_health *health, uint64_t start_us)
{
	memset(health->offences, 0, health->config->detection_window * sizeof(*health->offences));
	health->last_response_us = start_us;
	health->offence_index = 0U;
	health->offence_count = 0U;
}

void reflector_health_record_response(struct reflector_health *health, uint64_t timestamp_us)
{
	health->last_response_us = timestamp_us;
}

enum reflector_health_result
reflector_health_check(struct reflector_health *health, uint64_t timestamp_us)
{
	bool offence = interval_elapsed(
		timestamp_us,
		health->last_response_us,
		health->config->response_deadline_us
	);

	if (health->offences[health->offence_index] != 0U)
		health->offence_count--;
	health->offences[health->offence_index] = offence ? 1U : 0U;
	if (offence)
		health->offence_count++;
	health->offence_index++;
	if (health->offence_index == health->config->detection_window)
		health->offence_index = 0U;

	if (health->offence_count >= health->config->detection_threshold)
		return REFLECTOR_HEALTH_MISBEHAVING;
	return offence ? REFLECTOR_HEALTH_OFFENCE : REFLECTOR_HEALTH_OK;
}

void reflector_compare(
	const struct latency_tracker *trackers,
	const size_t *reflector_order,
	size_t active_count,
	struct reflector_comparison *comparisons
)
{
	int64_t minimum_baseline_us = INT64_MAX;
	int64_t minimum_delta_ewma_us[DIRECTION_COUNT] = { INT64_MAX, INT64_MAX };
	size_t index;

	/* Each reflector's own values first, then the minimums over all of them. */
	for (index = 0U; index < active_count; index++) {
		const struct latency_tracker *tracker = &trackers[reflector_order[index]];
		const struct latency_direction_tracker *directions = tracker->directions;
		struct reflector_comparison *comparison = &comparisons[index];

		comparison->sum_owd_baselines_us = signed_sum(
			directions[DIRECTION_DOWNLOAD].baseline_us,
			directions[DIRECTION_UPLOAD].baseline_us
		);
		minimum_baseline_us =
			min_i64(minimum_baseline_us, comparison->sum_owd_baselines_us);
		for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
			comparison->delta_ewma_us[direction] = directions[direction].delta_ewma_us;
			minimum_delta_ewma_us[direction] =
				min_i64(minimum_delta_ewma_us[direction],
					comparison->delta_ewma_us[direction]);
		}
	}

	for (index = 0U; index < active_count; index++) {
		struct reflector_comparison *comparison = &comparisons[index];

		comparison->minimum_sum_owd_baselines_us = minimum_baseline_us;
		comparison->sum_owd_baselines_delta_us =
			absolute_difference(comparison->sum_owd_baselines_us, minimum_baseline_us);
		for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
			comparison->minimum_delta_ewma_us[direction] =
				minimum_delta_ewma_us[direction];
			comparison->delta_ewma_delta_us[direction] = signed_difference(
				comparison->delta_ewma_us[direction],
				minimum_delta_ewma_us[direction]
			);
		}
	}
}

void reflector_rotate(
	size_t *reflector_order,
	size_t reflector_count,
	size_t active_count,
	size_t pinger
)
{
	size_t bad_reflector = reflector_order[pinger];
	size_t *standby = &reflector_order[active_count];

	/* The first standby takes the slot; the bad reflector queues last. */
	reflector_order[pinger] = standby[0];
	memmove(standby, standby + 1, (reflector_count - active_count - 1U) * sizeof(*standby));
	reflector_order[reflector_count - 1U] = bad_reflector;
}
