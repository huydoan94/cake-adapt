#define _POSIX_C_SOURCE 200809L

#include "helpers.h"

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
        strspn(start, "0123456789") != (size_t)(end - start)
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
    return value / 100U * percentage +
        (value % 100U * percentage + 99U) / 100U;
}

uint64_t rounded_divide(
    uint64_t value,
    uint64_t divisor
)
{
    return value / divisor +
        (value % divisor >= divisor / 2U + divisor % 2U ? 1U : 0U);
}

bool interval_elapsed(
    uint64_t current,
    uint64_t previous,
    uint64_t interval
)
{
    return current > previous && current - previous > interval;
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
    *timestamp = (uint64_t)value.tv_sec * 1000000U +
        (uint64_t)value.tv_nsec / 1000U;
    return true;
}

unsigned int load_percent(
    uint64_t traffic_rate,
    uint64_t shaper_rate
)
{
    uint64_t traffic_kbps = traffic_rate / 1000U;
    uint64_t shaper_kbps = shaper_rate / 1000U;
    uint64_t quotient;
    uint64_t remainder;
    uint64_t percentage;

    if (shaper_kbps == 0U) {
        return 0U;
    }
    quotient = traffic_kbps / shaper_kbps;
    if (quotient > UINT_MAX / 100U) {
        return UINT_MAX;
    }
    remainder = traffic_kbps % shaper_kbps;
    percentage = quotient * 100U;
    /* Whole kbit/s bounds remainder by UINT64_MAX / 1000: no overflow here. */
    percentage += remainder * 100U / shaper_kbps;
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
        nanoseconds += 1000000000L;
    }
    if (
        seconds < 0 ||
        (uint64_t)seconds > UINT64_MAX / 1000U
    ) {
        return false;
    }
    *elapsed = (uint64_t)seconds * 1000U + (uint64_t)nanoseconds / 1000000U;
    return *elapsed > 0U;
}

uint64_t bits_per_second(
    uint64_t byte_delta,
    uint64_t elapsed_ms
)
{
    /* 8,000 converts a byte delta over milliseconds to bits per second. */
    long double rate = (long double)byte_delta * 8000.0L /
        (long double)elapsed_ms;

    return rate >= (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)rate;
}
