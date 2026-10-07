#define _POSIX_C_SOURCE 200809L

#include "platform/traffic.h"

#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"

uint64_t traffic_compensated_interval_us(
	uint64_t configured_interval_us,
	uint64_t round_trip_serialization_us
)
{
	return max_u64(configured_interval_us, saturating_mul(round_trip_serialization_us, 10U));
}

void traffic_init(struct traffic_monitor *monitor)
{
	*monitor = (struct traffic_monitor){ 0 };
}

enum traffic_update_result traffic_update(
	struct traffic_monitor *monitor,
	const struct traffic_sample *sample,
	uint64_t *traffic_bps
)
{
	struct traffic_sample previous = monitor->previous_sample;
	bool has_previous = monitor->has_previous_sample;

	*traffic_bps = 0U;
	monitor->previous_sample = *sample;
	monitor->has_previous_sample = true;
	if (!has_previous)
		return TRAFFIC_UPDATE_BASELINE;

	if (!qdisc_same(&sample->qdisc, &previous.qdisc))
		return TRAFFIC_UPDATE_QDISC_REPLACED;

	if (sample->bytes < previous.bytes)
		return TRAFFIC_UPDATE_COUNTER_RESET;

	/* Shorter intervals are refreshes right after a sample, not a rate. */
	if (!interval_elapsed(sample->timestamp_us, previous.timestamp_us, MILLISECOND - 1U))
		return TRAFFIC_UPDATE_INVALID_INTERVAL;

	*traffic_bps =
		bps(sample->bytes - previous.bytes, sample->timestamp_us - previous.timestamp_us);
	return TRAFFIC_UPDATE_RATES;
}
