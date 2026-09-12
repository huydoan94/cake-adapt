#include "traffic.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct traffic_sample sample(
    uint64_t rx_bytes,
    uint64_t tx_bytes,
    time_t seconds,
    long nanoseconds
)
{
    return (struct traffic_sample) {
        .rx_bytes = rx_bytes,
        .tx_bytes = tx_bytes,
        .timestamp = {
            .tv_sec = seconds,
            .tv_nsec = nanoseconds
        }
    };
}

static void test_initial_sample_establishes_baseline(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_rates rates = { 0U, 0U };
    struct traffic_sample first = sample(100U, 200U, 10, 0L);

    traffic_monitor_init(&monitor);

    assert(traffic_monitor_update(
        &monitor,
        &first,
        &rates
    ) == TRAFFIC_UPDATE_BASELINE);
}

static void test_rates_are_calculated(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_rates rates = { 0U, 0U };
    struct traffic_sample first = sample(100U, 200U, 10, 0L);
    struct traffic_sample second = sample(1100U, 2200U, 11, 0L);

    traffic_monitor_init(&monitor);
    (void)traffic_monitor_update(
        &monitor,
        &first,
        &rates
    );

    assert(traffic_monitor_update(
        &monitor,
        &second,
        &rates
    ) == TRAFFIC_UPDATE_RATES);
    assert(rates.rx_bits_per_second == 8000U);
    assert(rates.tx_bits_per_second == 16000U);
}

static void test_subsecond_interval_is_supported(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_rates rates = { 0U, 0U };
    struct traffic_sample first = sample(0U, 0U, 10, 250000000L);
    struct traffic_sample second = sample(1000U, 500U, 10, 750000000L);

    traffic_monitor_init(&monitor);
    (void)traffic_monitor_update(
        &monitor,
        &first,
        &rates
    );
    (void)traffic_monitor_update(
        &monitor,
        &second,
        &rates
    );

    assert(rates.rx_bits_per_second == 16000U);
    assert(rates.tx_bits_per_second == 8000U);
}

static void test_counter_reset_creates_new_baseline(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_rates rates = { 0U, 0U };
    struct traffic_sample first = sample(1000U, 2000U, 10, 0L);
    struct traffic_sample reset = sample(100U, 200U, 11, 0L);
    struct traffic_sample next = sample(1100U, 2200U, 12, 0L);

    traffic_monitor_init(&monitor);
    (void)traffic_monitor_update(
        &monitor,
        &first,
        &rates
    );

    assert(traffic_monitor_update(
        &monitor,
        &reset,
        &rates
    ) == TRAFFIC_UPDATE_COUNTER_RESET);
    assert(traffic_monitor_update(
        &monitor,
        &next,
        &rates
    ) == TRAFFIC_UPDATE_RATES);
    assert(rates.rx_bits_per_second == 8000U);
    assert(rates.tx_bits_per_second == 16000U);
}

static void test_invalid_interval_creates_new_baseline(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_rates rates = { 0U, 0U };
    struct traffic_sample first = sample(100U, 200U, 10, 0L);
    struct traffic_sample same_time = sample(200U, 300U, 10, 0L);

    traffic_monitor_init(&monitor);
    (void)traffic_monitor_update(
        &monitor,
        &first,
        &rates
    );

    assert(traffic_monitor_update(
        &monitor,
        &same_time,
        &rates
    ) == TRAFFIC_UPDATE_INVALID_INTERVAL);
}

static void test_loopback_counters_can_be_read(void)
{
    struct traffic_sample current;
    char error[256] = "";

    assert(traffic_read(
        "lo",
        &current,
        error,
        sizeof(error)
    ) == 0);
}

static void test_missing_interface_reports_an_error(void)
{
    struct traffic_sample current;
    char error[256] = "";

    assert(traffic_read(
        "sqm-mon-missing",
        &current,
        error,
        sizeof(error)
    ) != 0);
    assert(strlen(error) > 0U);
}

int main(void)
{
    test_initial_sample_establishes_baseline();
    test_rates_are_calculated();
    test_subsecond_interval_is_supported();
    test_counter_reset_creates_new_baseline();
    test_invalid_interval_creates_new_baseline();
    test_loopback_counters_can_be_read();
    test_missing_interface_reports_an_error();

    (void)puts("traffic tests passed");
    return 0;
}
