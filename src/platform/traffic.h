#ifndef TRAFFIC_H_INCLUDED
#define TRAFFIC_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "platform/netlink.h"

struct traffic_sample {
	uint64_t bytes;
	/* Another qdisc's counter starts over, so a change re-baselines. */
	struct qdisc_id qdisc;
	uint64_t timestamp_us;
};

struct traffic_monitor {
	bool has_previous_sample;
	struct traffic_sample previous_sample;
};

enum traffic_update_result {
	TRAFFIC_UPDATE_BASELINE,
	TRAFFIC_UPDATE_RATES,
	TRAFFIC_UPDATE_COUNTER_RESET,
	TRAFFIC_UPDATE_QDISC_REPLACED,
	TRAFFIC_UPDATE_INVALID_INTERVAL
};

void traffic_init(struct traffic_monitor *monitor);

/*
 * Pinned upstream cadence: at least ten round trips of one wire packet each
 * way. Sampling itself still uses actual elapsed time.
 */
uint64_t traffic_compensated_interval_us(
	uint64_t configured_interval_us,
	uint64_t round_trip_serialization_us
);

enum traffic_update_result traffic_update(
	struct traffic_monitor *monitor,
	const struct traffic_sample *sample,
	uint64_t *traffic_bits_per_second
);

#endif
