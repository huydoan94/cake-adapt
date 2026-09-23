#define _POSIX_C_SOURCE 200809L

#include "cpu.h"

#include "error.h"
#include "constants.h"
#include "helpers.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t counter_sum(const struct cpu_counter *counter)
{
    /* Match cake-autorate: sum all ten counters, treating only IDLE as idle. */
    return counter->user + counter->nice + counter->system + counter->idle +
        counter->iowait + counter->irq + counter->softirq + counter->steal +
        counter->guest + counter->guest_nice;
}

void cpu_init(struct cpu_monitor *monitor)
{
    memset(monitor, 0, sizeof(*monitor));
}

int cpu_read(
    const char *path,
    struct cpu_sample *sample,
    char *error,
    size_t error_size
)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0U;
    int result = -1;

    if (!read_clock_microseconds(CLOCK_REALTIME, &sample->timestamp_microseconds)) {
        error_set(
            error,
            error_size,
            "could not read CPU timestamp: %s",
            strerror(errno)
        );
        return -1;
    }
    file = fopen(path, FILE_MODE_READ);
    if (file == NULL) {
        error_set(error, error_size, "could not open %s: %s", path, strerror(errno));
        return -1;
    }
    sample->count = 0U;
    while (getline(&line, &capacity, file) >= 0) {
        struct cpu_counter *counter;

        if (strncmp(line, CPU_PREFIX, sizeof(CPU_PREFIX) - 1U) != 0) {
            break;
        }
        if (sample->count == CPU_MAX_COUNT) {
            error_set(error, error_size, "more than %u CPU counters in %s", CPU_MAX_COUNT, path);
            goto done;
        }
        counter = &sample->counters[sample->count];
        memset(counter, 0, sizeof(*counter));
        if (
            sscanf(
                line,
                "%15s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                " %" SCNu64 " %" SCNu64,
                counter->identifier,
                &counter->user,
                &counter->nice,
                &counter->system,
                &counter->idle,
                &counter->iowait,
                &counter->irq,
                &counter->softirq,
                &counter->steal,
                &counter->guest,
                &counter->guest_nice
            ) < 5
        ) {
            error_set(error, error_size, "invalid CPU counters in %s", path);
            goto done;
        }
        sample->count++;
    }
    if (
        ferror(file) ||
        sample->count == 0U
    ) {
        error_set(error, error_size, "could not read CPU counters from %s", path);
        goto done;
    }
    result = 0;
done:
    free(line);
    (void)fclose(file);
    return result;
}

void cpu_usage(
    struct cpu_monitor *monitor,
    const struct cpu_sample *sample,
    unsigned int usage[CPU_MAX_COUNT]
)
{
    size_t index;

    for (index = 0U; index < sample->count; index++) {
        uint64_t sum = counter_sum(&sample->counters[index]);
        uint64_t delta = sum >= monitor->previous_sums[index]
            ? sum - monitor->previous_sums[index]
            : 0U;
        uint64_t idle = sample->counters[index].idle >=
                monitor->previous_idle[index]
            ? sample->counters[index].idle - monitor->previous_idle[index]
            : 0U;

        usage[index] = delta > idle
            ? (unsigned int)(100U * (delta - idle) / delta)
            : 0U;
        monitor->previous_sums[index] = sum;
        monitor->previous_idle[index] = sample->counters[index].idle;
    }
}
