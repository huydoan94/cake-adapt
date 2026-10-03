#include "controller/reflector.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int health_init(struct reflector_health *health, const struct reflector_health_config *config,
		uint64_t start_microseconds)
{
	if (health == NULL || config == NULL || config->detection_window == 0U ||
	    config->detection_threshold == 0U ||
	    config->detection_threshold > config->detection_window) {
		errno = EINVAL;
		return -1;
	}

	health->offences = calloc(config->detection_window, sizeof(*health->offences));
	if (health->offences == NULL)
		return -1;
	health->config = *config;
	health->last_response_microseconds = start_microseconds;
	health->offence_index = 0U;
	health->offence_count = 0U;
	return 0;
}

void health_cleanup(struct reflector_health *health)
{
	free(health->offences);
	health->offences = NULL;
	health->offence_index = 0U;
	health->offence_count = 0U;
}

void health_reset(struct reflector_health *health, uint64_t start_microseconds)
{
	memset(health->offences, 0, health->config.detection_window * sizeof(*health->offences));
	health->last_response_microseconds = start_microseconds;
	health->offence_index = 0U;
	health->offence_count = 0U;
}

void health_record_response(struct reflector_health *health, uint64_t timestamp_microseconds)
{
	health->last_response_microseconds = timestamp_microseconds;
}

enum reflector_health_result health_check(struct reflector_health *health,
					  uint64_t timestamp_microseconds)
{
	bool offence = interval_elapsed(timestamp_microseconds, health->last_response_microseconds,
					health->config.response_deadline_microseconds);

	if (health->offences[health->offence_index] != 0U)
		health->offence_count--;
	health->offences[health->offence_index] = offence ? 1U : 0U;
	if (offence)
		health->offence_count++;
	health->offence_index++;
	if (health->offence_index == health->config.detection_window)
		health->offence_index = 0U;

	if (health->offence_count >= health->config.detection_threshold)
		return REFLECTOR_MISBEHAVING;
	return offence ? REFLECTOR_OFFENCE : REFLECTOR_HEALTHY;
}

void reflector_compare(const struct latency_tracker *trackers, const size_t *reflector_order,
		       size_t active_count, struct reflector_comparison *comparisons)
{
	int64_t minimum_baseline;
	int64_t minimum_download_delta_ewma;
	int64_t minimum_upload_delta_ewma;
	size_t index;

	minimum_baseline = signed_sum(trackers[reflector_order[0]].download.baseline_microseconds,
				      trackers[reflector_order[0]].upload.baseline_microseconds);
	minimum_download_delta_ewma = trackers[reflector_order[0]].download.delta_ewma_microseconds;
	minimum_upload_delta_ewma = trackers[reflector_order[0]].upload.delta_ewma_microseconds;
	for (index = 1U; index < active_count; index++) {
		const struct latency_tracker *tracker = &trackers[reflector_order[index]];
		int64_t sum_baselines = signed_sum(tracker->download.baseline_microseconds,
						   tracker->upload.baseline_microseconds);

		if (sum_baselines < minimum_baseline)
			minimum_baseline = sum_baselines;
		if (tracker->download.delta_ewma_microseconds < minimum_download_delta_ewma)
			minimum_download_delta_ewma = tracker->download.delta_ewma_microseconds;
		if (tracker->upload.delta_ewma_microseconds < minimum_upload_delta_ewma)
			minimum_upload_delta_ewma = tracker->upload.delta_ewma_microseconds;
	}

	for (index = 0U; index < active_count; index++) {
		const struct latency_tracker *tracker = &trackers[reflector_order[index]];
		int64_t sum_baselines = signed_sum(tracker->download.baseline_microseconds,
						   tracker->upload.baseline_microseconds);
		int64_t download_delta_ewma = tracker->download.delta_ewma_microseconds;
		int64_t upload_delta_ewma = tracker->upload.delta_ewma_microseconds;

		comparisons[index] = (struct reflector_comparison){
			.minimum_sum_owd_baselines_microseconds = minimum_baseline,
			.sum_owd_baselines_microseconds = sum_baselines,
			.sum_owd_baselines_delta_microseconds =
				absolute_difference(sum_baselines, minimum_baseline),
			.minimum_download_delta_ewma_microseconds = minimum_download_delta_ewma,
			.download_delta_ewma_microseconds = download_delta_ewma,
			.download_delta_ewma_delta_microseconds =
				signed_difference(download_delta_ewma, minimum_download_delta_ewma),
			.minimum_upload_delta_ewma_microseconds = minimum_upload_delta_ewma,
			.upload_delta_ewma_microseconds = upload_delta_ewma,
			.upload_delta_ewma_delta_microseconds =
				signed_difference(upload_delta_ewma, minimum_upload_delta_ewma)
		};
	}
}

void reflector_rotate(size_t *reflector_order, size_t reflector_count, size_t active_count,
		      size_t pinger)
{
	size_t bad_reflector;

	bad_reflector = reflector_order[pinger];
	reflector_order[pinger] = reflector_order[active_count];
	memmove(&reflector_order[active_count], &reflector_order[active_count + 1U],
		(reflector_count - active_count - 1U) * sizeof(*reflector_order));
	reflector_order[reflector_count - 1U] = bad_reflector;
}
