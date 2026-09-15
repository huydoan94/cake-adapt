#define _POSIX_C_SOURCE 200809L

#include "traffic.h"
#include "helpers.h"

void traffic_init(struct sqm_mon_traffic_monitor *monitor)
{
    *monitor = (struct sqm_mon_traffic_monitor) { 0 };
}

enum traffic_update_result traffic_update(
    struct sqm_mon_traffic_monitor *monitor,
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
    if (!has_previous) {
        return TRAFFIC_UPDATE_BASELINE;
    }

    if (sample->qdisc_handle != previous.qdisc_handle ||
        sample->qdisc_parent != previous.qdisc_parent) {
        return TRAFFIC_UPDATE_QDISC_REPLACED;
    }

    if (sample->bytes < previous.bytes) {
        return TRAFFIC_UPDATE_COUNTER_RESET;
    }

    if (!elapsed_milliseconds(
            &previous.timestamp,
            &sample->timestamp,
            &elapsed
        )) {
        return TRAFFIC_UPDATE_INVALID_INTERVAL;
    }

    *rate_bits_per_second = bits_per_second(sample->bytes - previous.bytes, elapsed);
    return TRAFFIC_UPDATE_RATES;
}
