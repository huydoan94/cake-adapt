#ifndef COMMON_CONVERT_H_INCLUDED
#define COMMON_CONVERT_H_INCLUDED

#include "common/arith.h"
#include "constants/units.h"

#include <limits.h>
#include <stdint.h>
#include <time.h>

/*
 * Unit conversions, used only where a value enters or leaves the daemon. The
 * base units are bits and bytes, microseconds and ratios per million.
 */
static inline uint64_t byte_to_bit(uint64_t bytes)
{
	return saturating_mul(bytes, BITS_PER_BYTE);
}

static inline uint64_t bit_to_byte(uint64_t bits)
{
	return bits / BITS_PER_BYTE;
}

/* Whole kilobits, truncated; a kilobit is 1,000 bits. */
static inline uint64_t bit_to_kbit(uint64_t bits)
{
	return bits / KILOBIT;
}

static inline uint64_t kbit_to_bit(uint64_t kilobits)
{
	return saturating_mul(kilobits, KILOBIT);
}

/* Whole KB, truncated; a KB is 1,024 bytes. */
static inline uint64_t byte_to_kbyte(uint64_t bytes)
{
	return bytes / KILOBYTE;
}

static inline uint64_t kbyte_to_byte(uint64_t kilobytes)
{
	return saturating_mul(kilobytes, KILOBYTE);
}

/* Whole milliseconds for uloop: rounded up so a timer never wakes early, and saturated. */
static inline unsigned int us_to_ms(uint64_t us)
{
	uint64_t ms = us / MICROSECONDS_PER_MILLISECOND +
		      (us % MICROSECONDS_PER_MILLISECOND != 0U ? 1U : 0U);

	return ms > UINT_MAX ? UINT_MAX : (unsigned int)ms;
}

/* us_to_ms() for uloop timeouts and poll(), which take a signed int. */
static inline int us_to_timeout_ms(uint64_t us)
{
	unsigned int ms = us_to_ms(us);

	return ms > INT_MAX ? INT_MAX : (int)ms;
}

/* For messages that print a duration in seconds. */
static inline double us_to_sec_decimal(uint64_t us)
{
	return (double)us / (double)MICROSECONDS_PER_SECOND;
}

/* Kernel nanoseconds as microseconds, half a microsecond or more rounding up. */
static inline uint64_t ns_to_us(uint64_t ns)
{
	return rounded_divide(ns, NANOSECONDS_PER_MICROSECOND);
}

static inline int64_t signed_ns_to_us(int64_t ns)
{
	return signed_rounded_divide(ns, (int64_t)NANOSECONDS_PER_MICROSECOND);
}

/* A nonnegative clock reading in microseconds, rounded. */
static inline uint64_t timespec_to_us(const struct timespec *value)
{
	return (uint64_t)value->tv_sec * MICROSECONDS_PER_SECOND +
	       ns_to_us((uint64_t)value->tv_nsec);
}

/* Microseconds as a timespec, for clock deadlines. */
static inline struct timespec us_to_timespec(uint64_t us)
{
	return (struct timespec){
		.tv_sec = (time_t)(us / MICROSECONDS_PER_SECOND),
		.tv_nsec = (long)(us % MICROSECONDS_PER_SECOND * NANOSECONDS_PER_MICROSECOND),
	};
}

#endif
