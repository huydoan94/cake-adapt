#ifndef COMMON_ARITH_H_INCLUDED
#define COMMON_ARITH_H_INCLUDED

/* Trivial integer arithmetic shared by every module: saturation, rounding and ratios. */

#include "constants/units.h"

#include <stdint.h>

/* Same definition as libubox/utils.h, for modules that do not include it. */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#endif

static inline uint64_t saturating_add(uint64_t value, uint64_t addend)
{
	uint64_t sum;

	return __builtin_add_overflow(value, addend, &sum) ? UINT64_MAX : sum;
}

static inline uint64_t saturating_mul(uint64_t value, uint64_t factor)
{
	uint64_t product;

	return __builtin_mul_overflow(value, factor, &product) ? UINT64_MAX : product;
}

static inline uint64_t min_u64(uint64_t first, uint64_t second)
{
	return first < second ? first : second;
}

static inline uint64_t max_u64(uint64_t first, uint64_t second)
{
	return first > second ? first : second;
}

static inline int64_t max_i64(int64_t first, int64_t second)
{
	return first > second ? first : second;
}

static inline int64_t min_i64(int64_t first, int64_t second)
{
	return first < second ? first : second;
}

/* value - subtrahend, or zero when subtrahend is larger. */
static inline uint64_t saturating_sub(uint64_t value, uint64_t subtrahend)
{
	return value > subtrahend ? value - subtrahend : 0U;
}

/* first + second, saturated to the int64_t range. */
static inline int64_t signed_sum(int64_t first, int64_t second)
{
	int64_t sum;

	if (!__builtin_add_overflow(first, second, &sum))
		return sum;
	return first < 0 ? INT64_MIN : INT64_MAX;
}

/* Exact |first - second|; every int64_t difference fits in uint64_t. */
static inline uint64_t absolute_difference(int64_t first, int64_t second)
{
	if (first >= second)
		return (uint64_t)first - (uint64_t)second;
	return (uint64_t)second - (uint64_t)first;
}

/* first - second, saturated to the int64_t range. */
static inline int64_t signed_difference(int64_t first, int64_t second)
{
	uint64_t difference = absolute_difference(first, second);

	if (first >= second)
		return difference > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)difference;
	return difference > (uint64_t)INT64_MAX ? INT64_MIN : -(int64_t)difference;
}

/*
 * value * numerator / denominator, split so the full value is never
 * multiplied; exact while numerator <= denominator, as for shares and rates.
 */
static inline uint64_t mul_div(uint64_t value, uint64_t numerator, uint64_t denominator)
{
	return value / denominator * numerator + value % denominator * numerator / denominator;
}

/* Positive divisor; nearest integer, with ties rounded upward. */
static inline uint64_t rounded_divide(uint64_t value, uint64_t divisor)
{
	return value / divisor + (value % divisor >= divisor / 2U + divisor % 2U ? 1U : 0U);
}

/* Positive divisor; nearest integer, with ties rounded away from zero. */
static inline int64_t signed_rounded_divide(int64_t value, int64_t divisor)
{
	int64_t quotient = value / divisor;
	int64_t remainder = value % divisor;

	/* At least half way when the remainder is no smaller than what is left. */
	if (remainder > 0 && remainder >= divisor - remainder)
		return quotient + 1;
	if (remainder < 0 && -remainder >= divisor + remainder)
		return quotient - 1;
	return quotient;
}

/*
 * Ratios are integers per million: RATIO_ONE_E6 is 100%, and a factor or a load
 * may exceed it.
 */

/* value * ratio, truncated; exact while the ratio is at most UINT64_MAX / RATIO_ONE_E6. */
static inline uint64_t ratio_of(uint64_t value, uint64_t ratio_e6)
{
	return saturating_add(
		saturating_mul(value / RATIO_ONE_E6, ratio_e6),
		value % RATIO_ONE_E6 * ratio_e6 / RATIO_ONE_E6
	);
}

/* value * ratio, rounded upward; the same bound applies. */
static inline uint64_t ratio_of_rounded_up(uint64_t value, uint64_t ratio_e6)
{
	return saturating_add(
		saturating_mul(value / RATIO_ONE_E6, ratio_e6),
		(value % RATIO_ONE_E6 * ratio_e6 + RATIO_ONE_E6 - 1U) / RATIO_ONE_E6
	);
}

#endif
