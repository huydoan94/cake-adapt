#define _POSIX_C_SOURCE 200809L

#include "helpers.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void test_unsigned_decimal_spans(void)
{
    const char maximum[] = "18446744073709551615]";
    const char overflow[] = "18446744073709551616]";
    const char invalid[] = "+1]";
    const char empty[] = "]";
    uint64_t value = 123U;

    assert(parse_unsigned(maximum, maximum + strlen(maximum) - 1U, &value));
    assert(value == UINT64_MAX);
    assert(!parse_unsigned(overflow, overflow + strlen(overflow) - 1U, &value));
    assert(!parse_unsigned(invalid, invalid + strlen(invalid) - 1U, &value));
    assert(!parse_unsigned(empty, empty, &value));
    assert(value == UINT64_MAX);
}

static void test_percentages_and_rounding(void)
{
    assert(percentage_of(0U, 90U) == 0U);
    assert(percentage_of(1U, 90U) == 1U);
    assert(percentage_of(101U, 90U) == 91U);
    assert(percentage_of(UINT64_MAX, 100U) == UINT64_MAX);
    assert(percentage_of(UINT64_MAX, 1U) == UINT64_MAX / 100U + 1U);
    assert(rounded_divide(0U, 2U) == 0U);
    assert(rounded_divide(4U, 3U) == 1U);
    assert(rounded_divide(5U, 3U) == 2U);
    assert(rounded_divide(5U, 2U) == 3U);
    assert(rounded_divide(UINT64_MAX, UINT64_MAX) == 1U);
    assert(rounded_divide(UINT64_MAX / 2U, UINT64_MAX) == 0U);
}

static void test_elapsed_interval_boundaries(void)
{
    assert(!interval_elapsed(9U, 10U, 1U));
    assert(!interval_elapsed(10U, 10U, 0U));
    assert(!interval_elapsed(11U, 10U, 1U));
    assert(interval_elapsed(12U, 10U, 1U));
    assert(interval_elapsed(UINT64_MAX, 0U, UINT64_MAX - 1U));
}

static void test_load_rounding_and_limits(void)
{
    assert(load_percent(0U, 0U) == 0U);
    assert(load_percent(1000U, 999U) == 0U);
    assert(load_percent(750999U, 1000000U) == 75U);
    assert(load_percent(2000000U, 1000000U) == 200U);
    assert(load_percent(UINT64_MAX, 1000U) == UINT_MAX);
    assert(load_percent(UINT64_MAX, UINT64_MAX) == 100U);
    assert(load_percent(UINT64_MAX - 1000U, UINT64_MAX) == 99U);
}

static void test_clock_failure_preserves_output(void)
{
    uint64_t timestamp = 123U;

    assert(!read_clock_microseconds((clockid_t)-9999, &timestamp));
    assert(errno == EINVAL);
    assert(timestamp == 123U);
    assert(read_clock_microseconds(CLOCK_MONOTONIC, &timestamp));
    assert(timestamp > 0U);
}

static void test_rate_conversion_boundaries(void)
{
    assert(bits_per_second(0U, 1U) == 0U);
    assert(bits_per_second(1U, 3U) == 2666U);
    assert(bits_per_second(125000000U, 1000U) == 1000000000U);
    assert(bits_per_second(UINT64_MAX / 8000U, 1U) == UINT64_MAX / 8000U * 8000U);
    assert(bits_per_second(UINT64_MAX / 8000U + 1U, 1U) == UINT64_MAX);
    assert(bits_per_second(UINT64_MAX, UINT64_MAX) == 8000U);
}

int main(void)
{
    test_percentages_and_rounding();
    test_unsigned_decimal_spans();
    test_elapsed_interval_boundaries();
    test_load_rounding_and_limits();
    test_clock_failure_preserves_output();
    test_rate_conversion_boundaries();
    (void)puts("helper tests passed");
    return 0;
}
