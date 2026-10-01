#define _POSIX_C_SOURCE 200809L

#include "common/helpers.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

struct random_sequence {
    const uint32_t *values;
    size_t count;
    size_t index;
};

static bool sequence_u32(uint32_t *value, void *context)
{
    struct random_sequence *sequence = context;

    if (sequence->index == sequence->count) {
        errno = EIO;
        return false;
    }
    *value = sequence->values[sequence->index];
    sequence->index++;
    return true;
}

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

static void test_saturating_signed_arithmetic(void)
{
    assert(absolute_difference(3, -2) == 5U);
    assert(absolute_difference(-2, 3) == 5U);
    assert(absolute_difference(INT64_MAX, INT64_MIN) == UINT64_MAX);
    assert(signed_difference(3, 5) == -2);
    assert(signed_difference(INT64_MAX, -1) == INT64_MAX);
    assert(signed_difference(INT64_MIN, 1) == INT64_MIN);
    assert(signed_difference(-1, INT64_MAX) == INT64_MIN);
    assert(signed_sum(-3, 5) == 2);
    assert(signed_sum(INT64_MAX, 1) == INT64_MAX);
    assert(signed_sum(INT64_MIN, -1) == INT64_MIN);
}

static void test_milliseconds_round_up(void)
{
    assert(milliseconds_rounded_up(0U) == 0U);
    assert(milliseconds_rounded_up(1U) == 1U);
    assert(milliseconds_rounded_up(1000U) == 1U);
    assert(milliseconds_rounded_up(1001U) == 2U);
    assert(milliseconds_rounded_up(UINT64_MAX) == UINT64_MAX / 1000U + 1U);
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
    assert(bits_per_second(1U, 0U) == UINT64_MAX);
    assert(bits_per_second(0U, 1U) == 0U);
    assert(bits_per_second(1U, 3U) == 2666U);
    assert(bits_per_second(125000000U, 1000U) == 1000000000U);
    assert(bits_per_second(UINT64_MAX / 8000U, 1U) == UINT64_MAX / 8000U * 8000U);
    assert(bits_per_second(UINT64_MAX / 8000U + 1U, 1U) == UINT64_MAX);
    assert(bits_per_second(UINT64_MAX, UINT64_MAX) == 8000U);
}

static void test_serialization_microseconds(void)
{
    assert(serialization_microseconds(12000U, 1000000U) == 12000U);
    assert(serialization_microseconds(UINT64_MAX - 1U, UINT64_MAX) == UINT64_MAX);
    assert(serialization_microseconds(UINT64_MAX, 1U) == UINT64_MAX);
    assert(serialization_microseconds(12000U, 0U) == 0U);
}

static void test_random_selection_and_shuffle(void)
{
    const uint32_t choices[] = { UINT32_MAX, 8U };
    const uint32_t swaps[] = { 1U, 0U, 1U };
    const uint32_t partial[] = { 1U };
    struct random_sequence sequence = {
        .values = choices,
        .count = sizeof(choices) / sizeof(choices[0])
    };
    struct random_sequence shuffle_sequence = {
        .values = swaps,
        .count = sizeof(swaps) / sizeof(swaps[0])
    };
    struct random_sequence failing_sequence = {
        .values = partial,
        .count = sizeof(partial) / sizeof(partial[0])
    };
    size_t items[] = { 0U, 1U, 2U, 3U };
    size_t startup_order[] = { 0U, 1U, 2U, 3U };
    size_t working_order[] = { 0U, 1U, 2U, 3U };
    size_t index = SIZE_MAX;

    assert(random_below(3U, sequence_u32, &sequence, &index));
    assert(index == 2U);
    assert(sequence.index == 2U);
    assert(random_below(1U, sequence_u32, &sequence, &index));
    assert(index == 0U);
    assert(sequence.index == 2U);

    assert(shuffle(items, 4U, sequence_u32, &shuffle_sequence));
    assert(items[0] == 2U);
    assert(items[1] == 3U);
    assert(items[2] == 0U);
    assert(items[3] == 1U);

    /* Monitor shuffles a working copy and commits it only on success. */
    assert(!shuffle(working_order, 4U, sequence_u32, &failing_sequence));
    assert(
        memcmp(
            startup_order,
            (size_t[]) { 0U, 1U, 2U, 3U },
            sizeof(startup_order)
        ) == 0
    );
    assert(
        memcmp(
            working_order,
            (size_t[]) { 0U, 3U, 2U, 1U },
            sizeof(working_order)
        ) == 0
    );
    assert(shuffle(items, 1U, sequence_u32, &shuffle_sequence));
}

static void test_response_timestamp_boundaries(void)
{
    const uint64_t realtime = UINT64_C(10) * 1000000U;
    const uint64_t monotonic = UINT64_C(3) * 1000000U;
    uint64_t response_monotonic;
    bool stale;

    response_timestamp(
        realtime,
        monotonic,
        realtime - 499999U,
        &response_monotonic,
        &stale
    );
    assert(!stale);
    assert(response_monotonic == monotonic - 499999U);
    response_timestamp(
        realtime,
        monotonic,
        realtime - 500000U,
        &response_monotonic,
        &stale
    );
    assert(!stale);
    assert(response_monotonic == monotonic - 500000U);
    response_timestamp(
        realtime,
        monotonic,
        realtime - 500001U,
        &response_monotonic,
        &stale
    );
    assert(stale);
    assert(response_monotonic == monotonic - 500001U);
    response_timestamp(
        realtime,
        monotonic,
        realtime + 123U,
        &response_monotonic,
        &stale
    );
    assert(!stale);
    assert(response_monotonic == monotonic + 123U);
    response_timestamp(
        UINT64_MAX - 1U,
        UINT64_MAX - 1U,
        UINT64_MAX,
        &response_monotonic,
        &stale
    );
    assert(!stale);
    assert(response_monotonic == UINT64_MAX);
}

int main(void)
{
    test_percentages_and_rounding();
    test_unsigned_decimal_spans();
    test_saturating_signed_arithmetic();
    test_milliseconds_round_up();
    test_elapsed_interval_boundaries();
    test_load_rounding_and_limits();
    test_clock_failure_preserves_output();
    test_rate_conversion_boundaries();
    test_serialization_microseconds();
    test_random_selection_and_shuffle();
    test_response_timestamp_boundaries();
    (void)puts("helper tests passed");
    return 0;
}
