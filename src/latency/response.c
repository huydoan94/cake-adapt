#include "latency/response.h"
#include "common/arith.h"
#include "constants/latency.h"

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

bool response_stale(uint64_t now_us, uint64_t response_us)
{
	return saturating_sub(now_us, response_us) > LATENCY_STALE_RESPONSE_US;
}
