#ifndef HELPERS_H
#define HELPERS_H

#include <pthread.h>
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

/* Serialization time in microseconds, rounded; saturated on overflow. */
uint64_t serialization_us(uint64_t wire_packet_bits, uint64_t rate_bps);

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

bool read_clock_us(clockid_t clock_identifier, uint64_t *timestamp_us);

/*
 * Joins THREAD if it exits within bound_us of CLOCK_MONOTONIC, checking every 10 ms, so a
 * step of the wall clock cannot stretch the wait. The number of checks also bounds it when
 * the clock cannot be read. False leaves the thread running and joinable.
 */
bool thread_join_within(pthread_t thread, uint64_t bound_us);

/*
 * PATH as an absolute path in BUFFER: copied when it is absolute, otherwise
 * joined to the current directory (the path need not exist). Returns -1 with
 * errno set when the current directory is unknown or the result does not fit.
 */
int absolute_path(const char *path, char *buffer, size_t size);

/*
 * Takes an exclusive lock on PATH, creating it, and writes this process's pid
 * into it. Returns the descriptor, which holds the lock until it is closed or
 * the process exits, or -1 with errno set. EWOULDBLOCK means another process
 * holds the lock; holder_pid is then its pid, or 0 when that is unreadable.
 */
int lock_pid_file(const char *path, long *holder_pid);

/* A counter delta over a positive duration, in bit/s; saturated at UINT64_MAX. */
uint64_t bps(uint64_t byte_delta, uint64_t elapsed_us);

/* part / whole as a ratio per million, truncated and saturated; zero for an empty whole. */
uint64_t fraction_to_ratio_e6(uint64_t part, uint64_t whole);

/* traffic / shaper rate as a ratio; an unknown (zero) shaper rate gives no load. */
static inline uint64_t load_ratio_e6(uint64_t traffic_bps, uint64_t shaper_bps)
{
	return fraction_to_ratio_e6(traffic_bps, shaper_bps);
}

#endif
