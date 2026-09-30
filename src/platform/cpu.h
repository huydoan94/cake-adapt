#ifndef CPU_H
#define CPU_H

#include <stddef.h>
#include <stdint.h>

#define CPU_MAX_COUNT 65U
#define CPU_IDENTIFIER_SIZE 16U

struct cpu_counter {
    char identifier[CPU_IDENTIFIER_SIZE];
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
    uint64_t iowait;
    uint64_t irq;
    uint64_t softirq;
    uint64_t steal;
    uint64_t guest;
    uint64_t guest_nice;
};

struct cpu_sample {
    uint64_t timestamp_microseconds;
    size_t count;
    struct cpu_counter counters[CPU_MAX_COUNT];
};

struct cpu_monitor {
    uint64_t previous_sums[CPU_MAX_COUNT];
    uint64_t previous_idle[CPU_MAX_COUNT];
};

void cpu_init(struct cpu_monitor *monitor);

int cpu_read(
    const char *path,
    struct cpu_sample *sample,
    char *error,
    size_t error_size
);

void cpu_usage(
    struct cpu_monitor *monitor,
    const struct cpu_sample *sample,
    unsigned int usage[CPU_MAX_COUNT]
);

#endif
