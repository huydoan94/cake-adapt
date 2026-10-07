#ifndef CPU_H
#define CPU_H

#include <stddef.h>
#include <stdint.h>

#define CPU_MAX_COUNT 65U
#define CPU_IDENTIFIER_SIZE 16U

struct cpu_counter {
	char identifier[CPU_IDENTIFIER_SIZE];
	uint64_t user_ticks;
	uint64_t nice_ticks;
	uint64_t system_ticks;
	uint64_t idle_ticks;
	uint64_t iowait_ticks;
	uint64_t irq_ticks;
	uint64_t softirq_ticks;
	uint64_t steal_ticks;
	uint64_t guest_ticks;
	uint64_t guest_nice_ticks;
};

struct cpu_sample {
	uint64_t timestamp_us;
	size_t count;
	struct cpu_counter counters[CPU_MAX_COUNT];
};

/* Ticks spent busy over all ticks since the previous sample, for one CPU. */
struct cpu_busy {
	uint64_t busy_ticks;
	uint64_t total_ticks;
};

struct cpu_monitor {
	uint64_t previous_total_ticks[CPU_MAX_COUNT];
	uint64_t previous_idle_ticks[CPU_MAX_COUNT];
};

void cpu_init(struct cpu_monitor *monitor);

int cpu_read(const char *path, struct cpu_sample *sample, char *error, size_t error_size);

void cpu_usage(
	struct cpu_monitor *monitor,
	const struct cpu_sample *sample,
	struct cpu_busy usage[CPU_MAX_COUNT]
);

#endif
