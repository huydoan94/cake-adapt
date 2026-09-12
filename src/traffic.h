#ifndef SQM_MON_TRAFFIC_H
#define SQM_MON_TRAFFIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct traffic_sample {
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    struct timespec timestamp;
};

struct traffic_rates {
    uint64_t rx_bits_per_second;
    uint64_t tx_bits_per_second;
};

struct sqm_mon_traffic_monitor {
    bool has_previous_sample;
    struct traffic_sample previous_sample;
};

enum traffic_update_result {
    TRAFFIC_UPDATE_BASELINE,
    TRAFFIC_UPDATE_RATES,
    TRAFFIC_UPDATE_COUNTER_RESET,
    TRAFFIC_UPDATE_INVALID_INTERVAL
};

void traffic_monitor_init(struct sqm_mon_traffic_monitor *monitor);

int traffic_read(
    const char *interface,
    struct traffic_sample *sample,
    char *error,
    size_t error_size
);

enum traffic_update_result traffic_monitor_update(
    struct sqm_mon_traffic_monitor *monitor,
    const struct traffic_sample *sample,
    struct traffic_rates *rates
);

#endif
