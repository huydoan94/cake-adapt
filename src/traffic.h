#ifndef SQM_MON_TRAFFIC_H
#define SQM_MON_TRAFFIC_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct traffic_sample {
    uint64_t bytes;
    uint32_t qdisc_handle;
    uint32_t qdisc_parent;
    struct timespec timestamp;
};

struct sqm_mon_traffic_monitor {
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

void traffic_monitor_init(struct sqm_mon_traffic_monitor *monitor);

enum traffic_update_result traffic_monitor_update(
    struct sqm_mon_traffic_monitor *monitor,
    const struct traffic_sample *sample,
    uint64_t *rate_bits_per_second
);

#endif
