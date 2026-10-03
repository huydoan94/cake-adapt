#ifndef UTILS_H
#define UTILS_H

/* Trivial arithmetic shared by every module; anything longer is in helpers.c. */
#include <stdbool.h>
#include <stdint.h>

#include "common/constants.h"

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

/* first + second, saturated to the int64_t range. */
static inline int64_t signed_sum(int64_t first, int64_t second)
{
    int64_t sum;

    if (!__builtin_add_overflow(first, second, &sum)) {
        return sum;
    }
    return first < 0 ? INT64_MIN : INT64_MAX;
}

/* Exact |first - second|; every int64_t difference fits in uint64_t. */
static inline uint64_t absolute_difference(int64_t first, int64_t second)
{
    if (first >= second) {
        return (uint64_t)first - (uint64_t)second;
    }
    return (uint64_t)second - (uint64_t)first;
}

/* first - second, saturated to the int64_t range. */
static inline int64_t signed_difference(int64_t first, int64_t second)
{
    uint64_t difference = absolute_difference(first, second);

    if (first >= second) {
        return difference > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)difference;
    }
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

/* percentage is 0..100; rounded upward without multiplying the full value. */
static inline uint64_t percentage_of(uint64_t value, unsigned int percentage)
{
    return value / PERCENT * percentage + (value % PERCENT * percentage + PERCENT - 1U) / PERCENT;
}

/* Positive divisor; nearest integer, with ties rounded upward. */
static inline uint64_t rounded_divide(uint64_t value, uint64_t divisor)
{
    return value / divisor + (value % divisor >= divisor / 2U + divisor % 2U ? 1U : 0U);
}

/* Whole milliseconds, rounded up so a timer or poll never wakes early. */
static inline uint64_t milliseconds_rounded_up(uint64_t microseconds)
{
    return microseconds / MICROSECONDS_PER_MILLISECOND +
        (microseconds % MICROSECONDS_PER_MILLISECOND != 0U ? 1U : 0U);
}

/* Strict boundary: equality and backwards timestamps have not elapsed. */
static inline bool interval_elapsed(uint64_t current, uint64_t previous, uint64_t interval)
{
    return current > previous && current - previous > interval;
}

#endif
