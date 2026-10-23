#ifndef MONITOR_PINGERS_H_INCLUDED
#define MONITOR_PINGERS_H_INCLUDED

/* pingers.c: the pinger session, its children and its restart timing. */

#include "latency/latency.h"
#include "latency/native.h"
#include "constants/options.h"

#include <libubox/uloop.h>
#include <libubox/ustream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct monitor;

/* One pinger child: uloop drives both its output stream and its exit. */
struct pinger_watch {
	struct ustream_fd output;
	struct uloop_process process;
	struct monitor *monitor;
	size_t child_index;
	bool reading;
};

struct native_watch {
	struct uloop_fd socket;
	struct monitor *monitor;
	size_t channel;
};

struct monitor_pingers {
	struct latency latency;
	struct native_watch native_watches[NATIVE_CHANNELS];
	struct uloop_timeout native_timer;
	struct pinger_watch watches[CONFIG_MAX_REFLECTORS];
	struct uloop_timeout stop_timer;
	struct uloop_timeout failure_timer;
	struct uloop_timeout start_timer;
	bool observation_failed;
	bool response_clock_failed;
	/* Stopped while CAKE was missing; restarted with setup grace. */
	bool suspended;
	uint64_t last_response_us;
	uint64_t last_restart_us;
	uint64_t next_attempt_us;
	uint64_t grace_until_us;
};

void pingers_prepare(struct monitor *monitor, uint64_t start_us);

void pingers_close(struct monitor *monitor);

void pingers_stop_now(struct monitor *monitor);

bool pingers_watch(struct monitor *monitor);

/* CAKE is missing: stop the pingers until pingers_unsuspend(). */
void pingers_suspend(struct monitor *monitor);

void pingers_unsuspend(struct monitor *monitor, uint64_t timestamp_us);

/* Leaving IDLE: start again with setup grace. */
void pingers_resume(struct monitor *monitor, uint64_t timestamp_us);

/* Restart now with the current reflectors, as after a rotation. */
void pingers_reopen(struct monitor *monitor);

void pingers_restart(struct monitor *monitor, uint64_t timestamp_us);

#endif
