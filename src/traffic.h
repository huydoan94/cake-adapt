#ifndef TRAFFIC_H_INCLUDED
#define TRAFFIC_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct traffic_sample {
    uint64_t bytes;
    uint32_t qdisc_handle;
    uint32_t qdisc_parent;
    struct timespec timestamp;
};

struct traffic_monitor {
    bool has_previous_sample;
    struct traffic_sample previous_sample;
};

enum traffic_update_result {
    TRAFFIC_UPDATE_BASELINE,
    TRAFFIC_UPDATE_RATES,
    TRAFFIC_UPDATE_COUNTER_RESET,
    TRAFFIC_UPDATE_QDISC_REPLACED,
    TRAFFIC_UPDATE_INVALID_INTERVAL
};

void traffic_init(struct traffic_monitor *monitor);

enum traffic_update_result traffic_update(
    struct traffic_monitor *monitor,
    const struct traffic_sample *sample,
    uint64_t *rate_bits_per_second
);

#endif
