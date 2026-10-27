#ifndef DAEMON_PINGERS_H_INCLUDED
#define DAEMON_PINGERS_H_INCLUDED

/* pingers.c: the pinger session, its children and its restart timing. */

#include "latency/latency.h"
#include "latency/parser.h"
#include "latency/native.h"
#include "constants/options.h"

#include <libubox/uloop.h>
#include <libubox/ustream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct daemon;

/* One pinger child: uloop drives both its output stream and its exit. */
struct pinger_child_watch {
	struct ustream_fd output;
	struct uloop_process process;
	struct daemon *daemon;
	size_t child_index;
	bool reading;
};

struct pinger_socket_watch {
	struct uloop_fd socket;
	struct daemon *daemon;
	size_t channel;
};

struct daemon_pingers {
	struct latency latency;
	/* native_pinger.c: the native ICMP sockets and probe timer. */
	struct pinger_socket_watch native_watches[NATIVE_CHANNEL_COUNT];
	struct uloop_timeout native_timer;
	/* child_pingers.c: the fping and irtt children, their stop and staggered start. */
	struct pinger_child_watch watches[CONFIG_MAX_REFLECTORS];
	struct uloop_timeout stop_timer;
	struct uloop_timeout start_timer;
	/* pingers.c: the session, its retries and setup grace. */
	struct uloop_timeout failure_timer;
	bool observation_failed;
	bool response_clock_failed;
	/* Stopped while CAKE was missing; restarted with setup grace. */
	bool suspended;
	uint64_t last_response_us;
	uint64_t last_restart_us;
	uint64_t next_attempt_us;
	uint64_t grace_until_us;
};

void pingers_prepare(struct daemon *daemon, uint64_t start_us);

void pingers_close(struct daemon *daemon);

void pingers_stop_now(struct daemon *daemon);

bool pingers_watch(struct daemon *daemon);

/* CAKE is missing: stop the pingers until pingers_unsuspend(). */
void pingers_suspend(struct daemon *daemon);

void pingers_unsuspend(struct daemon *daemon, uint64_t timestamp_us);

/* Leaving IDLE: start again with setup grace. */
void pingers_resume(struct daemon *daemon, uint64_t timestamp_us);

/* Restart now with the current reflectors, as after a rotation. */
void pingers_reopen(struct daemon *daemon);

void pingers_restart(struct daemon *daemon, uint64_t timestamp_us);

/*
 * Within the pinger part: the session (pingers.c) and the two kinds of pinger
 * it drives, native probes (native_pinger.c) and pinger children
 * (child_pingers.c).
 */

/* pingers.c */

/* Logged once, until the session works again. */
void pingers_report_degraded(struct daemon *daemon, const char *error);

void pingers_report_failure(struct daemon *daemon, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/* Closes the session; the next attempt waits for interface_up_check_interval_s. */
void pingers_defer_retry(struct daemon *daemon);

/*
 * One reply into its reflector's tracker and the controller. captured_response_us
 * is a native probe's kernel receive time, or NULL for a pinger line's own
 * timestamp. Returns false when latency observation must be restarted.
 */
bool pingers_process_sample(
	struct daemon *daemon,
	const struct latency_sample *sample,
	const uint64_t *captured_response_us
);

/* One line of a pinger child's output; false when it must be restarted. */
bool pingers_process_line(struct daemon *daemon, size_t child_index, const char *line);

/* native_pinger.c */

void native_pinger_prepare(struct daemon *daemon);

/* Registers the session's sockets and starts the probe timer. */
bool native_pinger_watch(struct daemon *daemon);

void native_pinger_unwatch(struct daemon_pingers *pingers);

/* child_pingers.c */

void child_pingers_prepare(struct daemon *daemon);

/* Every spawned child must be known to uloop before anything can stop it. */
void child_pingers_register(struct daemon *daemon);

/* Reads the output of every child started so far. */
bool child_pingers_watch(struct daemon *daemon);

void child_pingers_release_output(struct pinger_child_watch *watch);

/* Starts the irtt children that are due, and schedules the next start. */
bool child_pingers_start_irtt(struct daemon *daemon);

#endif
