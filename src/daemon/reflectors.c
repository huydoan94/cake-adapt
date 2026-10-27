#define _GNU_SOURCE

#include "daemon/state.h"

#include "common/arith.h"
#include "common/clock.h"
#include "common/convert.h"
#include "common/random.h"
#include "logging/log.h"
#include "logging/records.h"
#include "constants/logging.h"
#include "constants/options.h"
#include "constants/units.h"

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

/* The pinger slots, each polling one active reflector. */
static size_t active_count(const struct daemon *daemon)
{
	return (size_t)daemon->config->no_pingers;
}

/* The reflector a pinger slot (or a standby position past them) polls. */
static const char *reflector_name(const struct daemon *daemon, size_t slot)
{
	const struct daemon_reflectors *reflectors = &daemon->reflectors;

	return daemon->config->reflectors[reflectors->order[slot]];
}

void reflectors_active(const struct daemon *daemon, const char *targets[])
{
	size_t index;

	for (index = 0U; index < active_count(daemon); index++)
		targets[index] = reflector_name(daemon, index);
}

size_t reflectors_find(const struct daemon *daemon, const char *target)
{
	size_t index;

	for (index = 0U; index < active_count(daemon); index++)
		if (strcmp(reflector_name(daemon, index), target) == 0)
			return index;
	return SIZE_MAX;
}

/* The largest of a slot's replies within a second; the second before it is kept. */
static void
recent_delay_add(struct reflector_recent_delay *recent, int64_t delay_us, uint64_t timestamp_us)
{
	uint64_t sec = timestamp_us / MICROSECONDS_PER_SECOND;

	if (sec != recent->sec) {
		recent->previous_us = sec == recent->sec + 1U ? recent->current_us : 0;
		recent->current_us = delay_us;
		recent->sec = sec;
	} else {
		recent->current_us = max_i64(recent->current_us, delay_us);
	}
}

/* A slot's largest added delay over the current and previous second, if it replied in them. */
static bool
recent_delay_value(const struct reflector_recent_delay *recent, uint64_t sec, int64_t *delay_us)
{
	if (recent->sec == sec)
		*delay_us = max_i64(recent->current_us, recent->previous_us);
	else if (recent->sec + 1U == sec)
		*delay_us = recent->current_us;
	else
		return false;
	return true;
}

int64_t reflectors_recent_delay_us(const struct daemon *daemon, uint64_t timestamp_us)
{
	const struct daemon_reflectors *reflectors = &daemon->reflectors;
	uint64_t sec = timestamp_us / MICROSECONDS_PER_SECOND;
	int64_t values_us[CONFIG_MAX_REFLECTORS];
	size_t count = 0U;

	/* Insertion sort: there are only a few pinger slots. */
	for (size_t slot = 0U; slot < active_count(daemon); slot++) {
		int64_t value_us;
		size_t position;

		if (!recent_delay_value(&reflectors->recent[slot], sec, &value_us))
			continue;
		for (position = count; position > 0U && values_us[position - 1U] > value_us;
		     position--)
			values_us[position] = values_us[position - 1U];
		values_us[position] = value_us;
		count++;
	}
	return count == 0U ? -1 : values_us[(count - 1U) / 2U];
}

void reflectors_record(
	struct daemon *daemon,
	size_t slot,
	const struct latency_sample *sample,
	uint64_t response_us,
	struct latency_observation *observation
)
{
	struct daemon_reflectors *reflectors = &daemon->reflectors;
	struct latency_tracker *tracker = &reflectors->trackers[reflectors->order[slot]];

	tracker_update(tracker, sample, observation);
	tracker_update_delta_ewma(tracker, shaper_low_load(daemon), observation);
	recent_delay_add(
		&reflectors->recent[slot],
		max_i64(observation->owd_delta_us[DIRECTION_DOWNLOAD] +
				observation->owd_delta_us[DIRECTION_UPLOAD],
			0),
		response_us
	);
	reflector_health_record_response(&reflectors->health[slot], response_us);
}

void reflectors_reset_health(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_reflectors *reflectors = &daemon->reflectors;
	size_t index;

	for (index = 0U; index < active_count(daemon); index++)
		reflector_health_reset(&reflectors->health[index], timestamp_us);
}

static void replace_active_reflector(struct daemon *daemon, size_t pinger, uint64_t timestamp_us)
{
	struct daemon_reflectors *reflectors = &daemon->reflectors;
	const struct config *config = daemon->config;
	size_t active = active_count(daemon);
	size_t reflector_count = (size_t)config->reflector_count;
	size_t bad_index = reflectors->order[pinger];
	const char *bad = config->reflectors[bad_index];
	bool rotate = reflector_count > active;

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
			reflector_name(daemon, active)
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
		reflector_rotate(reflectors->order, reflector_count, active, pinger);
	}
	reflector_health_reset(&reflectors->health[pinger], timestamp_us);
	log_message(
		LOG_LEVEL_DEBUG,
		"Resetting reflector offences associated with reflector: %s.",
		reflector_name(daemon, pinger)
	);
	/* One pinger session (native sockets or an fping process) owns all active targets. */
	if (rotate)
		pingers_reopen(daemon);
}

static bool compare_active_reflectors(struct daemon *daemon, uint64_t timestamp_us)
{
	const struct config *config = daemon->config;
	struct daemon_reflectors *reflectors = &daemon->reflectors;
	struct reflector_comparison comparisons[CONFIG_MAX_REFLECTORS];
	size_t active = active_count(daemon);
	size_t index;

	reflector_compare(reflectors->trackers, reflectors->order, active, comparisons);

	for (index = 0U; index < active; index++) {
		const struct reflector_comparison *comparison = &comparisons[index];
		const char *reflector = reflector_name(daemon, index);
		const char *column;

		if (config->output_reflector_stats) {
			/* Keep both upstream columns even though an echo pinger shares the delay. */
			struct log_reflector_record record = {
				.reflector = reflector,
				.minimum_sum_owd_baselines_us =
					comparison->minimum_sum_owd_baselines_us,
				.sum_owd_baselines_us = comparison->sum_owd_baselines_us,
				.sum_owd_baselines_delta_us =
					comparison->sum_owd_baselines_delta_us,
				.sum_owd_baselines_delta_threshold_us =
					config->reflector_sum_owd_baselines_delta_threshold_us,
				.delta_ewma_delta_threshold_us =
					config->reflector_owd_delta_ewma_delta_threshold_us,
			};

			for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
				record.minimum_delta_ewma_us[direction] =
					comparison->minimum_delta_ewma_us[direction];
				record.delta_ewma_us[direction] =
					comparison->delta_ewma_us[direction];
				record.delta_ewma_delta_us[direction] =
					comparison->delta_ewma_delta_us[direction];
			}
			log_reflector(&record);
		}

		if (comparison->sum_owd_baselines_delta_us >
		    config->reflector_sum_owd_baselines_delta_threshold_us)
			column = REFLECTOR_SUM_OWD_BASELINES;
		else if ((uint64_t)comparison->delta_ewma_delta_us[DIRECTION_DOWNLOAD] >
			 config->reflector_owd_delta_ewma_delta_threshold_us)
			column = REFLECTOR_DL_OWD_DELTA_EWMA;
		else if ((uint64_t)comparison->delta_ewma_delta_us[DIRECTION_UPLOAD] >
			 config->reflector_owd_delta_ewma_delta_threshold_us)
			column = REFLECTOR_UL_OWD_DELTA_EWMA;
		else
			continue;

		log_message(
			LOG_LEVEL_DEBUG,
			"Warning: reflector: %s %s exceeds the minimum by set threshold.",
			reflector,
			column
		);
		replace_active_reflector(daemon, index, timestamp_us);
		return true;
	}
	return false;
}

static bool run_scheduled_reflector_work(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_reflectors *reflectors = &daemon->reflectors;
	uint64_t replacement_interval_us = daemon->config->reflector_replacement_interval_us;
	uint64_t comparison_interval_us = daemon->config->reflector_comparison_interval_us;
	size_t pinger;

	if (interval_elapsed(
		    timestamp_us,
		    reflectors->last_replacement_us,
		    replacement_interval_us
	    )) {
		reflectors->last_replacement_us = timestamp_us;
		if (!random_below(active_count(daemon), entropy_u32, NULL, &pinger)) {
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
			reflector_name(daemon, pinger)
		);
		replace_active_reflector(daemon, pinger, timestamp_us);
		return true;
	}

	if (interval_elapsed(timestamp_us, reflectors->last_comparison_us, comparison_interval_us)) {
		reflectors->last_comparison_us = timestamp_us;
		return compare_active_reflectors(daemon, timestamp_us);
	}
	return false;
}

static void handle_health_timer(struct uloop_interval *timer)
{
	struct daemon *daemon =
		__extension__ container_of(timer, struct daemon, reflectors.health_timer);
	const struct config *config = daemon->config;
	struct daemon_reflectors *reflectors = &daemon->reflectors;
	const struct controller_activity *activity = &daemon->activity;
	const struct daemon_pingers *pingers = &daemon->pingers;
	uint64_t timestamp_us;
	bool reflector_replaced = false;
	size_t index;

	if (!links_ready(daemon) || activity->state != CONTROLLER_ACTIVITY_RUNNING)
		return;

	if (!read_clock_us(CLOCK_MONOTONIC, &timestamp_us)) {
		daemon_degraded(
			&reflectors->clock_failed,
			"reflector health check degraded: monotonic clock failed: %s",
			strerror(errno)
		);
		return;
	}
	daemon_recovered(
		&reflectors->clock_failed,
		"reflector health check recovered: monotonic clock available"
	);
	if (timestamp_us < pingers->grace_until_us)
		return;
	if (run_scheduled_reflector_work(daemon, timestamp_us))
		return;

	for (index = 0U; index < active_count(daemon); index++) {
		struct reflector_health *health = &reflectors->health[index];
		enum reflector_health_result result = reflector_health_check(health, timestamp_us);
		const char *reflector = reflector_name(daemon, index);

		if (result == REFLECTOR_HEALTH_OK)
			continue;
		log_message(
			LOG_LEVEL_DEBUG,
			"no ping response from reflector: %s within"
			" reflector_response_deadline: %.3fs",
			reflector,
			us_to_sec_decimal(config->reflector_response_deadline_us)
		);
		log_message(
			LOG_LEVEL_DEBUG,
			"reflector=%s, sum_reflector_offences=%zu and"
			" reflector_misbehaving_detection_thr=%" PRIu64,
			reflector,
			health->offence_count,
			config->reflector_misbehaving_detection_threshold
		);
		if (result == REFLECTOR_HEALTH_MISBEHAVING) {
			log_message(
				LOG_LEVEL_DEBUG,
				"Warning: reflector: %s seems to be misbehaving.",
				reflector
			);
			if (!reflector_replaced) {
				replace_active_reflector(daemon, index, timestamp_us);
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

int reflectors_start(struct daemon *daemon, uint64_t start_us)
{
	const struct config *config = daemon->config;
	struct daemon_reflectors *reflectors = &daemon->reflectors;
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
	reflectors->health_timer = (struct uloop_interval){ .cb = handle_health_timer };
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
	for (index = 0U; index < active_count(daemon); index++) {
		if (reflector_health_init(
			    &reflectors->health[index],
			    &reflectors->health_config,
			    start_us
		    ) != 0) {
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
void reflectors_stop(struct daemon *daemon)
{
	struct daemon_reflectors *reflectors = &daemon->reflectors;
	size_t index;

	for (index = 0U; index < active_count(daemon); index++)
		reflector_health_cleanup(&reflectors->health[index]);
}
