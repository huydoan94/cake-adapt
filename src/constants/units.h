#ifndef CONSTANTS_UNITS_H_INCLUDED
#define CONSTANTS_UNITS_H_INCLUDED

/* Base units and conversions. */

#include <stdint.h>

/*
 * Base units: bits and bytes (rates in bit/s, sizes in bytes, a KB being 1,024
 * bytes), microseconds for every time and duration, rounded to the nearest
 * microsecond, and ratios as integers per million. Other units appear only
 * where a value enters or leaves the daemon.
 */
#define THOUSAND UINT64_C(1000)
#define MILLION (THOUSAND * THOUSAND)
#define PERCENT UINT64_C(100)

/* Ratios are integers per million: RATIO_ONE_E6 is 100%. */
/* Ratios are integers per million: RATIO_ONE_E6 is 100%. */
#define RATIO_ONE_E6 MILLION
#define RATIO_PERCENT_E6 (RATIO_ONE_E6 / PERCENT)

/* Data and rates, in bits. A kilobit is 1,000 bits, as CAKE and tc count them. */
#define KILOBIT THOUSAND
#define MEGABIT (THOUSAND * KILOBIT)
#define BITS_PER_BYTE UINT64_C(8)

/* Sizes. A KB is 1,024 bytes, as cake-autorate's log limit and /proc count them. */
#define KILOBYTE UINT64_C(1024)

/* Durations. */
#define MICROSECOND UINT64_C(1)
#define MILLISECOND (THOUSAND * MICROSECOND)
#define SECOND (THOUSAND * MILLISECOND)
#define MINUTE (UINT64_C(60) * SECOND)
#define MICROSECONDS_PER_MILLISECOND (MILLISECOND / MICROSECOND)
#define MICROSECONDS_PER_SECOND SECOND
#define MICROSECONDS_PER_MINUTE MINUTE
#define NANOSECONDS_PER_MICROSECOND THOUSAND
#define NANOSECONDS_PER_MILLISECOND (THOUSAND * NANOSECONDS_PER_MICROSECOND)
#define NANOSECONDS_PER_SECOND (THOUSAND * NANOSECONDS_PER_MILLISECOND)

#endif
