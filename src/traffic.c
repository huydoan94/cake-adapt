#define _POSIX_C_SOURCE 200809L

#include "traffic.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TRAFFIC_COUNTER_PATH_SIZE \
    (sizeof("/sys/class/net//statistics/rx_bytes") + IF_NAMESIZE)

static void set_error(
    char *error,
    size_t error_size,
    const char *format,
    ...
)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static int read_counter(
    const char *interface,
    const char *counter_name,
    uint64_t *value,
    char *error,
    size_t error_size
)
{
    char path[TRAFFIC_COUNTER_PATH_SIZE];
    FILE *file;
    int next_character;
    int path_length;
    int scan_result;

    path_length = snprintf(
        path,
        sizeof(path),
        "/sys/class/net/%s/statistics/%s",
        interface,
        counter_name
    );
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        set_error(error, error_size, "interface name is too long");
        return -1;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        set_error(
            error,
            error_size,
            "could not open %s: %s",
            path,
            strerror(errno)
        );
        return -1;
    }

    scan_result = fscanf(file, "%" SCNu64, value);
    if (scan_result != 1) {
        set_error(error, error_size, "could not read a counter from %s", path);
        (void)fclose(file);
        return -1;
    }

    do {
        next_character = fgetc(file);
    } while (next_character != EOF &&
        isspace((unsigned char)next_character) != 0);

    if (next_character != EOF) {
        set_error(error, error_size, "counter in %s contains extra data", path);
        (void)fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        set_error(
            error,
            error_size,
            "could not close %s: %s",
            path,
            strerror(errno)
        );
        return -1;
    }

    return 0;
}

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
            .rx_bytes = 0U,
            .tx_bytes = 0U,
            .timestamp = {
                .tv_sec = 0,
                .tv_nsec = 0L
            }
        }
    };
}

int traffic_read(
    const char *interface,
    struct traffic_sample *sample,
    char *error,
    size_t error_size
)
{
    if (read_counter(
            interface,
            "rx_bytes",
            &sample->rx_bytes,
            error,
            error_size
        ) != 0) {
        return -1;
    }

    if (read_counter(
            interface,
            "tx_bytes",
            &sample->tx_bytes,
            error,
            error_size
        ) != 0) {
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &sample->timestamp) != 0) {
        set_error(
            error,
            error_size,
            "could not read the monotonic clock: %s",
            strerror(errno)
        );
        return -1;
    }

    return 0;
}

enum traffic_update_result traffic_monitor_update(
    struct sqm_mon_traffic_monitor *monitor,
    const struct traffic_sample *sample,
    struct traffic_rates *rates
)
{
    uint64_t elapsed;
    uint64_t rx_delta;
    uint64_t tx_delta;

    if (!monitor->has_previous_sample) {
        monitor->previous_sample = *sample;
        monitor->has_previous_sample = true;
        return TRAFFIC_UPDATE_BASELINE;
    }

    if (sample->rx_bytes < monitor->previous_sample.rx_bytes ||
        sample->tx_bytes < monitor->previous_sample.tx_bytes) {
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

    rx_delta = sample->rx_bytes - monitor->previous_sample.rx_bytes;
    tx_delta = sample->tx_bytes - monitor->previous_sample.tx_bytes;

    rates->rx_bits_per_second = bits_per_second(rx_delta, elapsed);
    rates->tx_bits_per_second = bits_per_second(tx_delta, elapsed);
    monitor->previous_sample = *sample;
    return TRAFFIC_UPDATE_RATES;
}
