#define _POSIX_C_SOURCE 200809L

#include "platform/traffic.h"

#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <limits.h>

uint64_t traffic_compensated_interval_microseconds(
	uint64_t configured_interval_microseconds,
	uint64_t download_wire_packet_bits,
	uint64_t download_rate_bits_per_second,
	uint64_t upload_wire_packet_bits,
	uint64_t upload_rate_bits_per_second
)
{
	uint64_t round_trip =
		serialization_microseconds(download_wire_packet_bits, download_rate_bits_per_second);
	uint64_t upload =
		serialization_microseconds(upload_wire_packet_bits, upload_rate_bits_per_second);

	round_trip = saturating_mul(saturating_add(round_trip, upload), 10U);
	return configured_interval_microseconds > round_trip ? configured_interval_microseconds :
							       round_trip;
}

void traffic_init(struct traffic_monitor *monitor)
{
	*monitor = (struct traffic_monitor){ 0 };
}

unsigned int traffic_interval_milliseconds(uint64_t interval_microseconds)
{
	uint64_t milliseconds = milliseconds_rounded_up(interval_microseconds);

	return milliseconds > UINT_MAX ? UINT_MAX : (unsigned int)milliseconds;
}

enum traffic_update_result traffic_update(
	struct traffic_monitor *monitor,
	const struct traffic_sample *sample,
	uint64_t *rate_bits_per_second
)
{
	struct traffic_sample previous = monitor->previous_sample;
	bool has_previous = monitor->has_previous_sample;
	uint64_t elapsed;

	*rate_bits_per_second = 0U;
	monitor->previous_sample = *sample;
	monitor->has_previous_sample = true;
	if (!has_previous)
		return TRAFFIC_UPDATE_BASELINE;

	if (sample->qdisc_handle != previous.qdisc_handle ||
	    sample->qdisc_parent != previous.qdisc_parent) {
		return TRAFFIC_UPDATE_QDISC_REPLACED;
	}

	if (sample->bytes < previous.bytes)
		return TRAFFIC_UPDATE_COUNTER_RESET;

	if (!elapsed_milliseconds(&previous.timestamp, &sample->timestamp, &elapsed))
		return TRAFFIC_UPDATE_INVALID_INTERVAL;

	*rate_bits_per_second = bits_per_second(sample->bytes - previous.bytes, elapsed);
	return TRAFFIC_UPDATE_RATES;
}
