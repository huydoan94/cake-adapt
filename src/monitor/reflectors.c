#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

static bool entropy_u32(uint32_t *value, void *context)
{
	(void)context;
	return getentropy(value, sizeof(*value)) == 0;
}

/* The reflector a pinger slot (or a standby position past them) polls. */
static const char *reflector_name(const struct monitor *monitor, size_t slot)
{
	return monitor->config->reflectors[monitor->reflectors.order[slot]];
}

void reflectors_active(const struct monitor *monitor, const char *targets[])
{
	size_t index;

	for (index = 0U; index < (size_t)monitor->config->no_pingers; index++)
		targets[index] = reflector_name(monitor, index);
}

size_t reflectors_find(const struct monitor *monitor, const char *target)
{
	size_t index;

	for (index = 0U; index < (size_t)monitor->config->no_pingers; index++)
		if (strcmp(reflector_name(monitor, index), target) == 0)
			return index;
	return SIZE_MAX;
}

/* The largest of a slot's replies within a second; the second before it is kept. */
static void
recent_delay_add(struct reflector_recent_delay *recent, int64_t delay_us, uint64_t timestamp_us)
{
	uint64_t second = timestamp_us / SECOND;

	if (second != recent->second) {
		recent->previous_us = second == recent->second + 1U ? recent->current_us : 0;
		recent->current_us = delay_us;
		recent->second = second;
	} else {
		recent->current_us = max_i64(recent->current_us, delay_us);
	}
}

/* A slot's largest added delay over the current and previous second, if it replied in them. */
static bool
recent_delay_value(const struct reflector_recent_delay *recent, uint64_t second, int64_t *delay_us)
{
	if (recent->second == second)
		*delay_us = max_i64(recent->current_us, recent->previous_us);
	else if (recent->second + 1U == second)
		*delay_us = recent->current_us;
	else
		return false;
	return true;
}

int64_t reflectors_recent_delay(const struct monitor *monitor, uint64_t timestamp_us)
{
	const struct monitor_reflectors *reflectors = &monitor->reflectors;
	const struct config *config = monitor->config;
	uint64_t second = timestamp_us / SECOND;
	int64_t values[CONFIG_MAX_REFLECTORS];
	size_t count = 0U;

	/* Insertion sort: there are only a few pinger slots. */
	for (size_t slot = 0U; slot < (size_t)config->no_pingers; slot++) {
		int64_t value;
		size_t position;

		if (!recent_delay_value(&reflectors->recent[slot], second, &value))
			continue;
		for (position = count; position > 0U && values[position - 1U] > value; position--)
			values[position] = values[position - 1U];
		values[position] = value;
		count++;
	}
	return count == 0U ? -1 : values[(count - 1U) / 2U];
}

void reflectors_record(
	struct monitor *monitor,
	size_t slot,
	const struct latency_sample *sample,
	uint64_t response_us,
	struct latency_observation *observation
)
{
	struct monitor_reflectors *reflectors = &monitor->reflectors;
	struct latency_tracker *tracker = &reflectors->trackers[reflectors->order[slot]];

	tracker_update(tracker, sample, observation);
	tracker_update_delta_ewma(tracker, control_low_load(monitor), observation);
	recent_delay_add(
		&reflectors->recent[slot],
		max_i64(observation->download_owd_delta_us + observation->upload_owd_delta_us, 0),
		response_us
	);
	health_record_response(&reflectors->health[slot], response_us);
}

void reflectors_reset_health(struct monitor *monitor, uint64_t timestamp_us)
{
	const struct config *config = monitor->config;
	size_t index;

	for (index = 0U; index < (size_t)config->no_pingers; index++)
		health_reset(&monitor->reflectors.health[index], timestamp_us);
}

static void replace_active_reflector(struct monitor *monitor, size_t pinger, uint64_t timestamp_us)
{
	struct monitor_reflectors *reflectors = &monitor->reflectors;
	const struct config *config = monitor->config;
	size_t active_count = (size_t)config->no_pingers;
	size_t reflector_count = (size_t)config->reflector_count;
	size_t bad_index = reflectors->order[pinger];
	const char *bad = config->reflectors[bad_index];
	bool rotate = reflector_count > active_count;

	if (!rotate) {
		log_message(
			LOG_LEVEL_DEBUG,
			"No additional reflectors specified so just retaining: %s.",
			bad
		);
	} else {
		log_message(
			LOG_LEVEL_DEBUG,
			"replacing reflector: %s with %s.",
			bad,
			reflector_name(monitor, active_count)
		);
		if (config->retain_reflector_stats) {
			log_message(
				LOG_LEVEL_DEBUG,
				"Retaining reflector stats associated with: %s",
				bad
			);
		} else {
			log_message(
				LOG_LEVEL_DEBUG,
				"Discarding reflector stats associated with %s",
				bad
			);
			tracker_reset(&reflectors->trackers[bad_index]);
		}
		reflector_rotate(reflectors->order, reflector_count, active_count, pinger);
	}
	health_reset(&reflectors->health[pinger], timestamp_us);
	log_message(
		LOG_LEVEL_DEBUG,
		"Resetting reflector offences associated with reflector: %s.",
		reflector_name(monitor, pinger)
	);
	/* fping owns all active targets in one process, so rotate them together. */
	if (rotate)
		pingers_reopen(monitor);
}

static bool compare_active_reflectors(struct monitor *monitor, uint64_t timestamp_us)
{
	struct monitor_reflectors *reflectors = &monitor->reflectors;
	struct reflector_comparison comparisons[CONFIG_MAX_REFLECTORS];
	size_t active_count = (size_t)monitor->config->no_pingers;
	size_t index;

	reflector_compare(reflectors->trackers, reflectors->order, active_count, comparisons);

	for (index = 0U; index < active_count; index++) {
		const struct reflector_comparison *comparison = &comparisons[index];
		const char *reflector = reflector_name(monitor, index);
		const char *column;

		if (monitor->config->output_reflector_stats) {
			/* Keep both upstream columns even though fping shares the delay. */
			const struct log_reflector_record record = {
				.reflector = reflector,
				.minimum_sum_owd_baselines_us =
					comparison->minimum_sum_owd_baselines_us,
				.sum_owd_baselines_us = comparison->sum_owd_baselines_us,
				.sum_owd_baselines_delta_us =
					comparison->sum_owd_baselines_delta_us,
				.sum_owd_baselines_delta_threshold_us =
					monitor->config
						->reflector_sum_owd_baselines_delta_threshold_us,
				.minimum_download_delta_ewma_us =
					comparison->minimum_download_delta_ewma_us,
				.download_delta_ewma_us = comparison->download_delta_ewma_us,
				.download_delta_ewma_delta_us =
					comparison->download_delta_ewma_delta_us,
				.delta_ewma_delta_threshold_us =
					monitor->config->reflector_owd_delta_ewma_delta_threshold_us,
				.minimum_upload_delta_ewma_us =
					comparison->minimum_upload_delta_ewma_us,
				.upload_delta_ewma_us = comparison->upload_delta_ewma_us,
				.upload_delta_ewma_delta_us =
					comparison->upload_delta_ewma_delta_us,
			};

			log_reflector(&record);
		}

		if (comparison->sum_owd_baselines_delta_us >
		    monitor->config->reflector_sum_owd_baselines_delta_threshold_us)
			column = REFLECTOR_SUM_OWD_BASELINES;
		else if ((uint64_t)comparison->download_delta_ewma_delta_us >
			 monitor->config->reflector_owd_delta_ewma_delta_threshold_us)
			column = REFLECTOR_DL_OWD_DELTA_EWMA;
		else if ((uint64_t)comparison->upload_delta_ewma_delta_us >
			 monitor->config->reflector_owd_delta_ewma_delta_threshold_us)
			column = REFLECTOR_UL_OWD_DELTA_EWMA;
		else
			continue;

		log_message(
			LOG_LEVEL_DEBUG,
			"Warning: reflector: %s %s exceeds the minimum by set threshold.",
			reflector,
			column
		);
		replace_active_reflector(monitor, index, timestamp_us);
		return true;
	}
	return false;
}

static bool run_scheduled_reflector_work(struct monitor *monitor, uint64_t timestamp_us)
{
	struct monitor_reflectors *reflectors = &monitor->reflectors;
	uint64_t replacement_interval_us = monitor->config->reflector_replacement_interval_us;
	uint64_t comparison_interval_us = monitor->config->reflector_comparison_interval_us;
	size_t pinger;

	if (interval_elapsed(
		    timestamp_us,
		    reflectors->last_replacement_us,
		    replacement_interval_us
	    )) {
		reflectors->last_replacement_us = timestamp_us;
		if (!random_below((size_t)monitor->config->no_pingers, entropy_u32, NULL, &pinger)) {
			log_message(
				LOG_LEVEL_WARNING,
				"could not randomly select reflector for replacement: %s",
				strerror(errno)
			);
			return false;
		}
		log_message(
			LOG_LEVEL_DEBUG,
			"reflector: %s randomly selected for replacement.",
			reflector_name(monitor, pinger)
		);
		replace_active_reflector(monitor, pinger, timestamp_us);
		return true;
	}

	if (interval_elapsed(timestamp_us, reflectors->last_comparison_us, comparison_interval_us)) {
		reflectors->last_comparison_us = timestamp_us;
		return compare_active_reflectors(monitor, timestamp_us);
	}
	return false;
}

static void handle_health_timer(struct uloop_interval *timer)
{
	struct monitor *monitor =
		__extension__ container_of(timer, struct monitor, reflectors.health_timer);
	struct monitor_reflectors *reflectors = &monitor->reflectors;
	uint64_t timestamp_us;
	bool reflector_replaced = false;
	size_t index;

	if (!links_ready(monitor) || monitor->activity.state != CONTROLLER_RUNNING)
		return;

	if (!read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		if (!reflectors->clock_failed) {
			log_message(
				LOG_LEVEL_WARNING,
				"reflector health check degraded: monotonic clock failed: %s",
				strerror(errno)
			);
		}
		reflectors->clock_failed = true;
		return;
	}
	if (reflectors->clock_failed) {
		log_message(
			LOG_LEVEL_NOTICE,
			"reflector health check recovered: monotonic clock available"
		);
		reflectors->clock_failed = false;
	}
	if (timestamp_us < monitor->pingers.grace_until_us)
		return;
	if (run_scheduled_reflector_work(monitor, timestamp_us))
		return;

	for (index = 0U; index < (size_t)monitor->config->no_pingers; index++) {
		enum reflector_health_result result =
			health_check(&reflectors->health[index], timestamp_us);
		const char *reflector = reflector_name(monitor, index);

		if (result == REFLECTOR_HEALTHY)
			continue;
		log_message(
			LOG_LEVEL_DEBUG,
			"no ping response from reflector: %s within"
			" reflector_response_deadline: %.3fs",
			reflector,
			us_to_sec(monitor->config->reflector_response_deadline_us)
		);
		log_message(
			LOG_LEVEL_DEBUG,
			"reflector=%s, sum_reflector_offences=%zu and"
			" reflector_misbehaving_detection_thr=%" PRIu64,
			reflector,
			reflectors->health[index].offence_count,
			monitor->config->reflector_misbehaving_detection_threshold
		);
		if (result == REFLECTOR_MISBEHAVING) {
			log_message(
				LOG_LEVEL_DEBUG,
				"Warning: reflector: %s seems to be misbehaving.",
				reflector
			);
			if (!reflector_replaced) {
				replace_active_reflector(monitor, index, timestamp_us);
				reflector_replaced = true;
			} else {
				log_message(
					LOG_LEVEL_DEBUG,
					"Warning: skipping replacement of reflector: %s given"
					" prior replacement within this reflector health check"
					" cycle.",
					reflector
				);
			}
		}
	}
}

int reflectors_start(struct monitor *monitor, uint64_t start_us)
{
	const struct config *config = monitor->config;
	struct monitor_reflectors *reflectors = &monitor->reflectors;
	const struct latency_tracker_config tracker_config = {
		.alpha_baseline_increase_ratio_e6 = config->alpha_baseline_increase_ratio_e6,
		.alpha_baseline_decrease_ratio_e6 = config->alpha_baseline_decrease_ratio_e6,
		.alpha_delta_ewma_ratio_e6 = config->alpha_delta_ewma_ratio_e6,
	};
	size_t index;

	reflectors->health_config = (struct reflector_health_config){
		.response_deadline_us = config->reflector_response_deadline_us,
		.detection_window = (size_t)config->reflector_misbehaving_detection_window,
		.detection_threshold = (size_t)config->reflector_misbehaving_detection_threshold,
	};
	reflectors->health_timer.cb = handle_health_timer;
	reflectors->last_replacement_us = start_us;
	reflectors->last_comparison_us = start_us;
	for (index = 0U; index < (size_t)config->reflector_count; index++) {
		tracker_init(&reflectors->trackers[index], &tracker_config);
		reflectors->order[index] = index;
	}
	log_message(LOG_LEVEL_DEBUG, "Randomizing reflectors.");
	if (config->randomize_reflectors) {
		size_t randomized_order[CONFIG_MAX_REFLECTORS];
		size_t reflector_count = (size_t)config->reflector_count;
		size_t size = reflector_count * sizeof(*randomized_order);

		memcpy(randomized_order, reflectors->order, size);
		if (!shuffle(randomized_order, reflector_count, entropy_u32, NULL)) {
			log_message(
				LOG_LEVEL_WARNING,
				"could not randomize reflectors: %s; using configured order",
				strerror(errno)
			);
		} else {
			memcpy(reflectors->order, randomized_order, size);
		}
	}
	for (index = 0U; index < (size_t)config->no_pingers; index++) {
		if (health_init(&reflectors->health[index], &reflectors->health_config, start_us) !=
		    0) {
			log_message(
				LOG_LEVEL_ERROR,
				"could not initialize reflector health: %s",
				strerror(errno)
			);
			return -1;
		}
	}
	return 0;
}

/* Health windows that were never allocated are NULL in the zeroed context. */
void reflectors_stop(struct monitor *monitor)
{
	const struct config *config = monitor->config;
	size_t index;

	for (index = 0U; index < (size_t)config->no_pingers; index++)
		health_cleanup(&monitor->reflectors.health[index]);
}
