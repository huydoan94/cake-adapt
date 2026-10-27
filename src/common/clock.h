#ifndef COMMON_CLOCK_H_INCLUDED
#define COMMON_CLOCK_H_INCLUDED

/* Clock readings in microseconds. */

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

bool read_clock_us(clockid_t clock_identifier, uint64_t *timestamp_us);

/* Strict boundary: equality and backwards timestamps have not elapsed. */
static inline bool interval_elapsed(uint64_t current, uint64_t previous, uint64_t interval)
{
	return current > previous && current - previous > interval;
}

#endif
