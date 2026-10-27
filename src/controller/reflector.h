#ifndef CONTROLLER_REFLECTOR_H_INCLUDED
#define CONTROLLER_REFLECTOR_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "common/direction.h"
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
	REFLECTOR_HEALTH_OK,
	REFLECTOR_HEALTH_OFFENCE,
	REFLECTOR_HEALTH_MISBEHAVING
};

struct reflector_comparison {
	int64_t minimum_sum_owd_baselines_us;
	int64_t sum_owd_baselines_us;
	uint64_t sum_owd_baselines_delta_us;
	int64_t minimum_delta_ewma_us[DIRECTION_COUNT];
	int64_t delta_ewma_us[DIRECTION_COUNT];
	int64_t delta_ewma_delta_us[DIRECTION_COUNT];
};

/* config is borrowed for the health record's lifetime. */
int reflector_health_init(
	struct reflector_health *health,
	const struct reflector_health_config *config,
	uint64_t start_us
);

void reflector_health_cleanup(struct reflector_health *health);

void reflector_health_reset(struct reflector_health *health, uint64_t start_us);

void reflector_health_record_response(struct reflector_health *health, uint64_t timestamp_us);

enum reflector_health_result
reflector_health_check(struct reflector_health *health, uint64_t timestamp_us);

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
