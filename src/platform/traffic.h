#ifndef TRAFFIC_H_INCLUDED
#define TRAFFIC_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct traffic_sample {
	uint64_t bytes;
	uint32_t qdisc_handle;
	uint32_t qdisc_parent;
	struct timespec timestamp;
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

/* Pinned upstream cadence; sampling itself still uses actual elapsed time. */
uint64_t traffic_compensated_interval_microseconds(
	uint64_t configured_interval_microseconds,
	uint64_t download_wire_packet_bits,
	uint64_t download_rate_bits_per_second,
	uint64_t upload_wire_packet_bits,
	uint64_t upload_rate_bits_per_second
);

enum traffic_update_result traffic_update(
	struct traffic_monitor *monitor,
	const struct traffic_sample *sample,
	uint64_t *rate_bits_per_second
);

#endif
