#ifndef DAEMON_UPKEEP_H_INCLUDED
#define DAEMON_UPKEEP_H_INCLUDED

/* upkeep.c: CPU and memory records, the log file's timer and its signals. */

#include "platform/cpu.h"

#include <libubox/uloop.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct daemon;

struct daemon_upkeep {
	struct uloop_interval cpu_timer;
	struct uloop_interval memory_timer;
	struct uloop_interval log_timer;
	struct uloop_signal log_export_signal;
	struct uloop_signal log_reset_signal;
	struct cpu_monitor cpu_monitor;
	size_t cpu_count;
	bool cpu_observation_failed;
	bool memory_observation_failed;
	/* The highest resident size seen, the MEMORY record's peak. */
	uint64_t memory_peak_bytes;
};

/* The timers' and signals' handlers; first, so stopping is safe from any point. */
void upkeep_init(struct daemon *daemon);

/* Starts what the configuration asks for; a timer that fails only degrades. */
void upkeep_start(struct daemon *daemon);

void upkeep_stop(struct daemon *daemon);

#endif
