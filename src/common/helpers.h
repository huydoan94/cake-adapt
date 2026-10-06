#ifndef HELPERS_H
#define HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* Decimal token in a NUL-terminated string; end points to its delimiter. */
bool parse_unsigned(const char *start, const char *end, uint64_t *value);

typedef bool (*random_u32_source)(uint32_t *value, void *context);

/* Uniformly select an index below count using the supplied entropy source. */
bool random_below(size_t count, random_u32_source source, void *context, size_t *index);

/* Shuffle in place; callers needing failure atomicity provide a working copy. */
bool shuffle(size_t *items, size_t count, random_u32_source source, void *context);

/* Whole-microsecond serialization time, saturated on overflow. */
uint64_t serialization_microseconds(uint64_t wire_packet_bits, uint64_t rate_bits_per_second);

/*
 * Map an epoch response timestamp into the monotonic clock domain, keeping
 * its age (or a future timestamp's offset). This keeps health/activity timers
 * monotonic without changing response age.
 */
uint64_t response_monotonic_microseconds(
	uint64_t processing_realtime_microseconds,
	uint64_t processing_monotonic_microseconds,
	uint64_t response_realtime_microseconds
);

/* Strictly older than 500 ms when processed; a future timestamp is never stale. */
bool response_stale(
	uint64_t processing_realtime_microseconds,
	uint64_t response_realtime_microseconds
);

bool read_clock_microseconds(clockid_t clock_identifier, uint64_t *timestamp);

bool elapsed_milliseconds(
	const struct timespec *previous,
	const struct timespec *current,
	uint64_t *elapsed
);

/* Positive elapsed milliseconds; overflow is saturated at UINT64_MAX. */
uint64_t bits_per_second(uint64_t byte_delta, uint64_t elapsed_ms);

/* Autorate load is calculated from whole kbit/s, saturated at UINT_MAX. */
unsigned int load_percent(uint64_t traffic_rate, uint64_t shaper_rate);

#endif
