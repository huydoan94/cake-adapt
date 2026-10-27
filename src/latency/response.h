#ifndef LATENCY_RESPONSE_H_INCLUDED
#define LATENCY_RESPONSE_H_INCLUDED

/* Reply timestamps: their age, and their place on the monotonic clock. */

#include <stdbool.h>
#include <stdint.h>

/*
 * Map an epoch response timestamp into the monotonic clock domain, keeping
 * its age (or a future timestamp's offset). This keeps health/activity timers
 * monotonic without changing response age.
 */
uint64_t response_monotonic_us(
	uint64_t processing_realtime_us,
	uint64_t processing_monotonic_us,
	uint64_t response_realtime_us
);

/*
 * Strictly older than 500 ms at now_us, both on the same clock; a future
 * timestamp is never stale.
 */
bool response_stale(uint64_t now_us, uint64_t response_us);

#endif
