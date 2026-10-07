#define _POSIX_C_SOURCE 200809L

#include "common/helpers.h"
#include "common/constants.h"
#include "common/utils.h"

#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool parse_unsigned(const char *start, const char *end, uint64_t *value)
{
	char *tail;
	unsigned long long parsed;

	if (start == end || strspn(start, DECIMAL_DIGITS) != (size_t)(end - start))
		return false;
	errno = 0;
	parsed = strtoull(start, &tail, 10);
	if (errno == ERANGE || tail != end)
		return false;
	*value = (uint64_t)parsed;
	return true;
}

bool random_below(size_t count, random_u32_source source, void *context, size_t *index)
{
	uint32_t value;
	uint32_t limit;

	if (count == 0U || count > (size_t)UINT32_MAX || source == NULL || index == NULL) {
		errno = EINVAL;
		return false;
	}
	if (count == 1U) {
		*index = 0U;
		return true;
	}

	limit = UINT32_MAX - (UINT32_MAX % (uint32_t)count) - 1U;
	do {
		if (!source(&value, context))
			return false;
	} while (value > limit);

	*index = (size_t)(value % (uint32_t)count);
	return true;
}

bool shuffle(size_t *items, size_t count, random_u32_source source, void *context)
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

		if (!random_below(position, source, context, &replacement))
			return false;
		temporary = items[current];
		items[current] = items[replacement];
		items[replacement] = temporary;
	}
	return true;
}

uint64_t serialization_us(uint64_t wire_packet_bits, uint64_t rate_bps)
{
	uint64_t whole;
	uint64_t remainder;
	uint64_t fractional;

	if (rate_bps == 0U)
		return 0U;
	whole = wire_packet_bits / rate_bps;
	remainder = wire_packet_bits % rate_bps;
	if (whole > UINT64_MAX / MICROSECONDS_PER_SECOND ||
	    __builtin_mul_overflow(remainder, MICROSECONDS_PER_SECOND, &fractional)) {
		return UINT64_MAX;
	}
	whole *= MICROSECONDS_PER_SECOND;
	return saturating_add(whole, rounded_divide(fractional, rate_bps));
}

uint64_t response_monotonic_us(
	uint64_t processing_realtime_us,
	uint64_t processing_monotonic_us,
	uint64_t response_realtime_us
)
{
	if (response_realtime_us > processing_realtime_us) {
		return saturating_add(
			processing_monotonic_us,
			response_realtime_us - processing_realtime_us
		);
	}
	return saturating_sub(
		processing_monotonic_us,
		processing_realtime_us - response_realtime_us
	);
}

bool response_stale(uint64_t processing_realtime_us, uint64_t response_realtime_us)
{
	return saturating_sub(processing_realtime_us, response_realtime_us) >
	       LATENCY_STALE_RESPONSE_US;
}

bool read_clock_us(clockid_t clock_identifier, uint64_t *timestamp_us)
{
	struct timespec value;

	if (clock_gettime(clock_identifier, &value) != 0 || value.tv_sec < 0)
		return false;
	*timestamp_us = timespec_to_us(&value);
	return true;
}

uint64_t fraction_to_ratio_e6(uint64_t part, uint64_t whole)
{
	/*
	 * The remainder below is multiplied by RATIO_ONE_E6, so it must stay under
	 * UINT64_MAX / RATIO_ONE_E6: 18 Tbit/s, or 213 days in microseconds. A larger
	 * whole gives up its last six digits, which no real value has.
	 */
	if (whole > UINT64_MAX / RATIO_ONE_E6) {
		part /= RATIO_ONE_E6;
		whole /= RATIO_ONE_E6;
	}
	if (whole == 0U)
		return 0U;
	/* part / whole = quotient + remainder / whole, each scaled on its own. */
	return saturating_add(
		saturating_mul(part / whole, RATIO_ONE_E6),
		part % whole * RATIO_ONE_E6 / whole
	);
}

uint64_t bps(uint64_t byte_delta, uint64_t elapsed_us)
{
	const uint64_t scale = BITS_PER_BYTE * MICROSECONDS_PER_SECOND;
	uint64_t scaled;
	long double rate;

	if (elapsed_us == 0U)
		return UINT64_MAX;
	/* Normal counters need only integer arithmetic, including on soft-float CPUs. */
	if (!__builtin_mul_overflow(byte_delta, scale, &scaled))
		return scaled / elapsed_us;
	/* Keep the full-width fallback for exceptional counter jumps. */
	rate = (long double)byte_delta * (long double)scale / (long double)elapsed_us;

	return rate >= (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)rate;
}
