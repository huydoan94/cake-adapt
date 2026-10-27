#ifndef COMMON_RATE_H_INCLUDED
#define COMMON_RATE_H_INCLUDED

/* Rates from counters, serialization times and ratios per million. */

#include <stdint.h>

/* Serialization time in microseconds, rounded; saturated on overflow. */
uint64_t serialization_us(uint64_t wire_packet_bits, uint64_t rate_bps);

/* A counter delta over a positive duration, in bit/s; saturated at UINT64_MAX. */
uint64_t bps(uint64_t byte_delta, uint64_t elapsed_us);

/* part / whole as a ratio per million, truncated and saturated; zero for an empty whole. */
uint64_t fraction_to_ratio_e6(uint64_t part, uint64_t whole);

/* traffic / shaper rate as a ratio; an unknown (zero) shaper rate gives no load. */
static inline uint64_t load_ratio_e6(uint64_t traffic_bps, uint64_t shaper_bps)
{
	return fraction_to_ratio_e6(traffic_bps, shaper_bps);
}

#endif
