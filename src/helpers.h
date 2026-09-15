#ifndef HELPERS_H
#define HELPERS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Decimal token in a NUL-terminated string; end points to its delimiter. */
bool parse_unsigned(
    const char *start,
    const char *end,
    uint64_t *value
);

/* percentage is 0..100; rounded upward without multiplying the full value. */
uint64_t percentage_of(
    uint64_t value,
    unsigned int percentage
);

/* Positive divisor; nearest integer, with ties rounded upward. */
uint64_t rounded_divide(
    uint64_t value,
    uint64_t divisor
);

/* Strict boundary: equality and backwards timestamps have not elapsed. */
bool interval_elapsed(
    uint64_t current,
    uint64_t previous,
    uint64_t interval
);

bool read_clock_microseconds(
    clockid_t clock_identifier,
    uint64_t *timestamp
);

bool elapsed_milliseconds(
    const struct timespec *previous,
    const struct timespec *current,
    uint64_t *elapsed
);

/* Positive elapsed milliseconds; overflow is saturated at UINT64_MAX. */
uint64_t bits_per_second(
    uint64_t byte_delta,
    uint64_t elapsed_ms
);

/* Autorate load is calculated from whole kbit/s, saturated at UINT_MAX. */
unsigned int load_percent(
    uint64_t traffic_rate,
    uint64_t shaper_rate
);

#endif
