#include "traffic.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define QDISC_HANDLE 0x00010000U
#define QDISC_PARENT UINT32_MAX

static struct traffic_sample sample_with_qdisc(
    uint64_t bytes,
    uint32_t handle,
    uint32_t parent,
    time_t seconds,
    long nanoseconds
)
{
    return (struct traffic_sample) {
        .bytes = bytes,
        .qdisc_handle = handle,
        .qdisc_parent = parent,
        .timestamp = {
            .tv_sec = seconds,
            .tv_nsec = nanoseconds
        }
    };
}

static struct traffic_sample sample(
    uint64_t bytes,
    time_t seconds,
    long nanoseconds
)
{
    return sample_with_qdisc(
        bytes,
        QDISC_HANDLE,
        QDISC_PARENT,
        seconds,
        nanoseconds
    );
}

static void test_initial_sample_establishes_baseline(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_sample first = sample(100U, 10, 0L);
    uint64_t rate = 123U;

    traffic_init(&monitor);

    assert(traffic_update(
        &monitor,
        &first,
        &rate
    ) == TRAFFIC_UPDATE_BASELINE);
    assert(rate == 0U);
}

static void test_rate_is_calculated_from_cake_bytes(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_sample first = sample(100U, 10, 0L);
    struct traffic_sample second = sample(2100U, 11, 0L);
    uint64_t rate;

    traffic_init(&monitor);
    (void)traffic_update(&monitor, &first, &rate);

    assert(traffic_update(
        &monitor,
        &second,
        &rate
    ) == TRAFFIC_UPDATE_RATES);
    assert(rate == 16000U);
}

static void test_subsecond_interval_is_supported(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_sample first = sample(0U, 10, 250000000L);
    struct traffic_sample second = sample(1000U, 10, 750000000L);
    uint64_t rate;

    traffic_init(&monitor);
    (void)traffic_update(&monitor, &first, &rate);
    (void)traffic_update(&monitor, &second, &rate);

    assert(rate == 16000U);
}

static void test_counter_reset_creates_new_baseline(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_sample first = sample(2000U, 10, 0L);
    struct traffic_sample reset = sample(200U, 11, 0L);
    struct traffic_sample next = sample(1200U, 12, 0L);
    uint64_t rate;

    traffic_init(&monitor);
    (void)traffic_update(&monitor, &first, &rate);

    assert(traffic_update(
        &monitor,
        &reset,
        &rate
    ) == TRAFFIC_UPDATE_COUNTER_RESET);
    assert(rate == 0U);
    assert(traffic_update(
        &monitor,
        &next,
        &rate
    ) == TRAFFIC_UPDATE_RATES);
    assert(rate == 8000U);
}

static void test_qdisc_replacement_creates_new_baseline(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_sample first = sample(1000U, 10, 0L);
    struct traffic_sample replacement = sample_with_qdisc(
        2000U,
        0x00020000U,
        QDISC_PARENT,
        11,
        0L
    );
    struct traffic_sample next = sample_with_qdisc(
        3000U,
        0x00020000U,
        QDISC_PARENT,
        12,
        0L
    );
    uint64_t rate;

    traffic_init(&monitor);
    (void)traffic_update(&monitor, &first, &rate);

    assert(traffic_update(
        &monitor,
        &replacement,
        &rate
    ) == TRAFFIC_UPDATE_QDISC_REPLACED);
    assert(rate == 0U);
    assert(traffic_update(
        &monitor,
        &next,
        &rate
    ) == TRAFFIC_UPDATE_RATES);
    assert(rate == 8000U);
}

static void test_invalid_interval_creates_new_baseline(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_sample first = sample(100U, 10, 0L);
    struct traffic_sample same_time = sample(200U, 10, 0L);
    uint64_t rate;

    traffic_init(&monitor);
    (void)traffic_update(&monitor, &first, &rate);

    assert(traffic_update(
        &monitor,
        &same_time,
        &rate
    ) == TRAFFIC_UPDATE_INVALID_INTERVAL);
    assert(rate == 0U);
}

static void test_large_64_bit_counter_is_supported(void)
{
    struct sqm_mon_traffic_monitor monitor;
    struct traffic_sample first = sample(UINT64_C(1) << 40U, 10, 0L);
    struct traffic_sample second = sample(
        (UINT64_C(1) << 40U) + 125000000U,
        11,
        0L
    );
    uint64_t rate;

    traffic_init(&monitor);
    (void)traffic_update(&monitor, &first, &rate);
    assert(traffic_update(
        &monitor,
        &second,
        &rate
    ) == TRAFFIC_UPDATE_RATES);
    assert(rate == 1000000000U);
}

int main(void)
{
    test_initial_sample_establishes_baseline();
    test_rate_is_calculated_from_cake_bytes();
    test_subsecond_interval_is_supported();
    test_counter_reset_creates_new_baseline();
    test_qdisc_replacement_creates_new_baseline();
    test_invalid_interval_creates_new_baseline();
    test_large_64_bit_counter_is_supported();

    (void)puts("traffic tests passed");
    return 0;
}
