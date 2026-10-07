#ifndef REFLECTOR_H_INCLUDED
#define REFLECTOR_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "latency/tracker.h"

struct reflector_health_config {
	uint64_t response_deadline_us;
	size_t detection_window;
	size_t detection_threshold;
};

struct reflector_health {
	/* Shared by every slot; the threshold is validated between 1 and the window. */
	const struct reflector_health_config *config;
	unsigned char *offences;
	uint64_t last_response_us;
	size_t offence_index;
	size_t offence_count;
};

enum reflector_health_result {
	REFLECTOR_HEALTHY,
	REFLECTOR_OFFENCE,
	REFLECTOR_MISBEHAVING
};

struct reflector_comparison {
	int64_t minimum_sum_owd_baselines_us;
	int64_t sum_owd_baselines_us;
	uint64_t sum_owd_baselines_delta_us;
	int64_t minimum_download_delta_ewma_us;
	int64_t download_delta_ewma_us;
	int64_t download_delta_ewma_delta_us;
	int64_t minimum_upload_delta_ewma_us;
	int64_t upload_delta_ewma_us;
	int64_t upload_delta_ewma_delta_us;
};

/* config is borrowed for the health record's lifetime. */
int health_init(
	struct reflector_health *health,
	const struct reflector_health_config *config,
	uint64_t start_us
);

void health_cleanup(struct reflector_health *health);

void health_reset(struct reflector_health *health, uint64_t start_us);

void health_record_response(struct reflector_health *health, uint64_t timestamp_us);

enum reflector_health_result health_check(struct reflector_health *health, uint64_t timestamp_us);

/* Validated, nonempty active order; indices refer to initialized trackers. */
void reflector_compare(
	const struct latency_tracker *trackers,
	const size_t *reflector_order,
	size_t active_count,
	struct reflector_comparison *comparisons
);

/* Caller supplies an active pinger and at least one standby reflector. */
void reflector_rotate(
	size_t *reflector_order,
	size_t reflector_count,
	size_t active_count,
	size_t pinger
);

#endif
