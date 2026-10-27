#ifndef LATENCY_TRACKER_H_INCLUDED
#define LATENCY_TRACKER_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "common/direction.h"
#include "latency/parser.h"
#include "constants/units.h"

struct latency_observation {
	int64_t owd_us[DIRECTION_COUNT];
	int64_t owd_baseline_us[DIRECTION_COUNT];
	int64_t owd_delta_us[DIRECTION_COUNT];
	int64_t owd_delta_ewma_us[DIRECTION_COUNT];
};

/* EWMA weights of a new sample, as ratios: validated at most RATIO_ONE_E6. */
struct latency_tracker_config {
	uint64_t alpha_baseline_increase_ratio_e6;
	uint64_t alpha_baseline_decrease_ratio_e6;
	uint64_t alpha_delta_ewma_ratio_e6;
};

struct latency_direction_tracker {
	int64_t baseline_us;
	int64_t delta_ewma_us;
};

struct latency_tracker {
	struct latency_tracker_config config;
	struct latency_direction_tracker directions[DIRECTION_COUNT];
};

void tracker_init(struct latency_tracker *tracker, const struct latency_tracker_config *config);

void tracker_reset(struct latency_tracker *tracker);

void tracker_update(
	struct latency_tracker *tracker,
	const struct latency_sample *sample,
	struct latency_observation *observation
);

void tracker_update_delta_ewma(
	struct latency_tracker *tracker,
	bool low_load,
	struct latency_observation *observation
);

#endif
