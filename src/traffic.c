#define _POSIX_C_SOURCE 200809L

#include "traffic.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

static bool elapsed_milliseconds(
    const struct timespec *previous,
    const struct timespec *current,
    uint64_t *elapsed
)
{
    time_t seconds;
    long nanoseconds;

    seconds = current->tv_sec - previous->tv_sec;
    nanoseconds = current->tv_nsec - previous->tv_nsec;

    if (nanoseconds < 0L) {
        --seconds;
        nanoseconds += 1000000000L;
    }

    if (seconds < 0 ||
        (uint64_t)seconds > UINT64_MAX / 1000U) {
        return false;
    }

    *elapsed = (uint64_t)seconds * 1000U +
        (uint64_t)nanoseconds / 1000000U;
    return *elapsed > 0U;
}

static uint64_t bits_per_second(
    uint64_t byte_delta,
    uint64_t elapsed_milliseconds_value
)
{
    long double rate;

    rate = (long double)byte_delta * 8000.0L /
        (long double)elapsed_milliseconds_value;
    if (rate >= (long double)UINT64_MAX) {
        return UINT64_MAX;
    }

    return (uint64_t)rate;
}

void traffic_monitor_init(struct sqm_mon_traffic_monitor *monitor)
{
    *monitor = (struct sqm_mon_traffic_monitor) {
        .has_previous_sample = false,
        .previous_sample = {
            .bytes = 0U,
            .qdisc_handle = 0U,
            .qdisc_parent = 0U,
            .timestamp = {
                .tv_sec = 0,
                .tv_nsec = 0L
            }
        }
    };
}

enum traffic_update_result traffic_monitor_update(
    struct sqm_mon_traffic_monitor *monitor,
    const struct traffic_sample *sample,
    uint64_t *rate_bits_per_second
)
{
    uint64_t byte_delta;
    uint64_t elapsed;

    *rate_bits_per_second = 0U;
    if (!monitor->has_previous_sample) {
        monitor->previous_sample = *sample;
        monitor->has_previous_sample = true;
        return TRAFFIC_UPDATE_BASELINE;
    }

    if (sample->qdisc_handle != monitor->previous_sample.qdisc_handle ||
        sample->qdisc_parent != monitor->previous_sample.qdisc_parent) {
        monitor->previous_sample = *sample;
        return TRAFFIC_UPDATE_QDISC_REPLACED;
    }

    if (sample->bytes < monitor->previous_sample.bytes) {
        monitor->previous_sample = *sample;
        return TRAFFIC_UPDATE_COUNTER_RESET;
    }

    if (!elapsed_milliseconds(
            &monitor->previous_sample.timestamp,
            &sample->timestamp,
            &elapsed
        )) {
        monitor->previous_sample = *sample;
        return TRAFFIC_UPDATE_INVALID_INTERVAL;
    }

    byte_delta = sample->bytes - monitor->previous_sample.bytes;
    *rate_bits_per_second = bits_per_second(byte_delta, elapsed);
    monitor->previous_sample = *sample;
    return TRAFFIC_UPDATE_RATES;
}
