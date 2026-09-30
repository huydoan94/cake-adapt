#define _POSIX_C_SOURCE 200809L

#include "common/helpers.h"
#include "common/constants.h"

#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool parse_unsigned(
    const char *start,
    const char *end,
    uint64_t *value
)
{
    char *tail;
    unsigned long long parsed;

    if (
        start == end ||
        strspn(start, DECIMAL_DIGITS) != (size_t)(end - start)
    ) {
        return false;
    }
    errno = 0;
    parsed = strtoull(start, &tail, 10);
    if (
        errno == ERANGE ||
        tail != end
    ) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

uint64_t percentage_of(
    uint64_t value,
    unsigned int percentage
)
{
    /* Split before multiplying to avoid overflowing the full-width value. */
    return value / PERCENT * percentage +
        (value % PERCENT * percentage + PERCENT - 1U) / PERCENT;
}

uint64_t rounded_divide(
    uint64_t value,
    uint64_t divisor
)
{
    return value / divisor +
        (value % divisor >= divisor / 2U + divisor % 2U ? 1U : 0U);
}

bool random_below(
    size_t count,
    random_u32_source source,
    void *context,
    size_t *index
)
{
    uint32_t value;
    uint32_t limit;

    if (
        count == 0U || count > (size_t)UINT32_MAX ||
        source == NULL || index == NULL
    ) {
        errno = EINVAL;
        return false;
    }
    if (count == 1U) {
        *index = 0U;
        return true;
    }

    limit = UINT32_MAX - (UINT32_MAX % (uint32_t)count) - 1U;
    do {
        if (!source(&value, context)) {
            return false;
        }
    } while (value > limit);

    *index = (size_t)(value % (uint32_t)count);
    return true;
}

bool shuffle(
    size_t *items,
    size_t count,
    random_u32_source source,
    void *context
)
{
    size_t position;

    if (items == NULL || source == NULL) {
        errno = EINVAL;
        return false;
    }
    for (position = count; position > 1U; position--) {
        size_t replacement;
        size_t current = position - 1U;
        size_t temporary;

        if (!random_below(position, source, context, &replacement)) {
            return false;
        }
        temporary = items[current];
        items[current] = items[replacement];
        items[replacement] = temporary;
    }
    return true;
}

uint64_t serialization_microseconds(
    uint64_t wire_packet_bits,
    uint64_t rate_bits_per_second
)
{
    uint64_t whole;
    uint64_t remainder;
    uint64_t fractional;

    if (rate_bits_per_second == 0U) {
        return 0U;
    }
    whole = wire_packet_bits / rate_bits_per_second;
    remainder = wire_packet_bits % rate_bits_per_second;
    if (
        whole > UINT64_MAX / MICROSECONDS_PER_SECOND ||
        __builtin_mul_overflow(remainder, MICROSECONDS_PER_SECOND, &fractional)
    ) {
        return UINT64_MAX;
    }
    whole *= MICROSECONDS_PER_SECOND;
    fractional /= rate_bits_per_second;
    return UINT64_MAX - whole < fractional ? UINT64_MAX : whole + fractional;
}

bool interval_elapsed(
    uint64_t current,
    uint64_t previous,
    uint64_t interval
)
{
    return current > previous && current - previous > interval;
}

void response_timestamp(
    uint64_t processing_realtime_microseconds,
    uint64_t processing_monotonic_microseconds,
    uint64_t response_realtime_microseconds,
    uint64_t *response_monotonic_microseconds,
    bool *stale
)
{
    uint64_t age_microseconds = 0U;

    if (response_realtime_microseconds > processing_realtime_microseconds) {
        age_microseconds = response_realtime_microseconds -
            processing_realtime_microseconds;
        *stale = false;
        *response_monotonic_microseconds =
            UINT64_MAX - processing_monotonic_microseconds < age_microseconds
                ? UINT64_MAX
                : processing_monotonic_microseconds + age_microseconds;
        return;
    }
    if (processing_realtime_microseconds > response_realtime_microseconds) {
        age_microseconds = processing_realtime_microseconds -
            response_realtime_microseconds;
    }
    *stale = age_microseconds > LATENCY_STALE_RESPONSE_MICROSECONDS;
    *response_monotonic_microseconds =
        age_microseconds >= processing_monotonic_microseconds
            ? 0U
            : processing_monotonic_microseconds - age_microseconds;
}

bool read_clock_microseconds(
    clockid_t clock_identifier,
    uint64_t *timestamp
)
{
    struct timespec value;

    if (
        clock_gettime(clock_identifier, &value) != 0 ||
        value.tv_sec < 0
    ) {
        return false;
    }
    *timestamp = (uint64_t)value.tv_sec * MICROSECONDS_PER_SECOND +
        (uint64_t)value.tv_nsec / NANOSECONDS_PER_MICROSECOND;
    return true;
}

unsigned int load_percent(
    uint64_t traffic_rate,
    uint64_t shaper_rate
)
{
    uint64_t traffic_kbps = traffic_rate / KILOBIT;
    uint64_t shaper_kbps = shaper_rate / KILOBIT;
    uint64_t quotient;
    uint64_t remainder;
    uint64_t percentage;

    if (shaper_kbps == 0U) {
        return 0U;
    }
    quotient = traffic_kbps / shaper_kbps;
    if (quotient > UINT_MAX / PERCENT) {
        return UINT_MAX;
    }
    remainder = traffic_kbps % shaper_kbps;
    percentage = quotient * PERCENT;
    /* Whole kbit/s bounds remainder by UINT64_MAX / KILOBIT. */
    percentage += remainder * PERCENT / shaper_kbps;
    return percentage > UINT_MAX ? UINT_MAX : (unsigned int)percentage;
}

bool elapsed_milliseconds(
    const struct timespec *previous,
    const struct timespec *current,
    uint64_t *elapsed
)
{
    time_t seconds = current->tv_sec - previous->tv_sec;
    long nanoseconds = current->tv_nsec - previous->tv_nsec;

    if (nanoseconds < 0L) {
        --seconds;
        nanoseconds += NANOSECONDS_PER_SECOND;
    }
    if (
        seconds < 0 ||
        (uint64_t)seconds > UINT64_MAX / MILLISECONDS_PER_SECOND
    ) {
        return false;
    }
    *elapsed = (uint64_t)seconds * MILLISECONDS_PER_SECOND +
        (uint64_t)nanoseconds / NANOSECONDS_PER_MILLISECOND;
    return *elapsed > 0U;
}

uint64_t bits_per_second(
    uint64_t byte_delta,
    uint64_t elapsed_ms
)
{
    const uint64_t scale = BITS_PER_BYTE * MILLISECONDS_PER_SECOND;
    uint64_t scaled;
    long double rate;

    /* Preserve saturation for an interval that cannot represent a rate. */
    if (elapsed_ms == 0U) {
        return UINT64_MAX;
    }
    /* Normal counters need only integer arithmetic, including on soft-float CPUs. */
    if (!__builtin_mul_overflow(byte_delta, scale, &scaled)) {
        return scaled / elapsed_ms;
    }
    /* Keep the full-width fallback for exceptional counter jumps. */
    rate = (long double)byte_delta * (long double)scale /
        (long double)elapsed_ms;

    return rate >= (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)rate;
}
