#include "common/rate.h"
#include "common/arith.h"
#include "constants/units.h"

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
	const uint64_t byte_per_us_to_bps = BITS_PER_BYTE * MICROSECONDS_PER_SECOND;

	if (elapsed_us == 0U)
		return UINT64_MAX;
	/*
	 * byte_delta * 8,000,000 / elapsed_us, truncated to whole bit/s. Splitting
	 * byte_delta by elapsed_us keeps the product of the remainder in range for
	 * any interval below 26 days, so the result is exact without floating point.
	 */
	return saturating_add(
		saturating_mul(byte_delta / elapsed_us, byte_per_us_to_bps),
		saturating_mul(byte_delta % elapsed_us, byte_per_us_to_bps) / elapsed_us
	);
}
