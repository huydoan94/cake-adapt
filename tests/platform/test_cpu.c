#define _POSIX_C_SOURCE 200809L

#include "platform/cpu.h"
#include "common/helpers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_proc_stat_parsing(void)
{
	char path[] = "/tmp/sqm-mon-cpu-test-XXXXXX";
	char error[256] = "";
	struct cpu_sample sample;
	int descriptor = mkstemp(path);
	FILE *file;

	assert(descriptor >= 0);
	file = fdopen(descriptor, "w");
	assert(file != NULL);
	assert(fputs("cpu 1 2 3 4 5 6 7 8 9 10\n"
		     "cpu0 10 20 30 40\n"
		     "intr 100\n",
		     file) >= 0);
	assert(fclose(file) == 0);
	assert(cpu_read(path, &sample, error, sizeof(error)) == 0);
	assert(sample.timestamp_us > 0U);
	assert(sample.count == 2U);
	assert(strcmp(sample.counters[0].identifier, "cpu") == 0);
	assert(sample.counters[0].user_ticks == 1U);
	assert(sample.counters[0].idle_ticks == 4U);
	assert(sample.counters[0].guest_nice_ticks == 10U);
	assert(strcmp(sample.counters[1].identifier, "cpu0") == 0);
	assert(sample.counters[1].idle_ticks == 40U);
	assert(sample.counters[1].iowait_ticks == 0U);
	assert(sample.counters[1].guest_nice_ticks == 0U);
	assert(unlink(path) == 0);
	assert(cpu_read(path, &sample, error, sizeof(error)) != 0);
}

static void test_cpu_usage_matches_cake_autorate(void)
{
	struct cpu_monitor monitor;
	struct cpu_sample sample = {
		.count = 1U,
		.counters = { { .identifier = "cpu",
				.user_ticks = 10U,
				.idle_ticks = 70U,
				.iowait_ticks = 10U,
				.guest_ticks = 10U } },
	};
	struct cpu_busy usage[CPU_MAX_COUNT];

	cpu_init(&monitor);
	cpu_usage(&monitor, &sample, usage);
	assert(fraction_to_ratio_e6(usage[0].busy_ticks, usage[0].total_ticks) == 300000U);
	sample.counters[0].user_ticks = 30U;
	sample.counters[0].idle_ticks = 110U;
	sample.counters[0].iowait_ticks = 20U;
	sample.counters[0].guest_ticks = 20U;
	cpu_usage(&monitor, &sample, usage);
	assert(fraction_to_ratio_e6(usage[0].busy_ticks, usage[0].total_ticks) == 500000U);
	cpu_usage(&monitor, &sample, usage);
	assert(fraction_to_ratio_e6(usage[0].busy_ticks, usage[0].total_ticks) == 0U);
	sample.counters[0] = (struct cpu_counter){ .identifier = "cpu", .idle_ticks = 1U };
	cpu_usage(&monitor, &sample, usage);
	assert(fraction_to_ratio_e6(usage[0].busy_ticks, usage[0].total_ticks) == 0U);
}

int main(void)
{
	test_proc_stat_parsing();
	test_cpu_usage_matches_cake_autorate();
	(void)puts("CPU tests passed");
	return 0;
}
