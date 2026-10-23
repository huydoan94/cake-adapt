#ifndef MONITOR_PUBLISH_H_INCLUDED
#define MONITOR_PUBLISH_H_INCLUDED

/*
 * publish.c: the main thread's side of the status object on ubus. A thread of
 * its own serves it from shared, so the main thread never calls libubus
 * beyond ubus_strerror().
 */

#include <pthread.h>

struct monitor;

struct publish_shared;

struct monitor_publish {
	/* NULL while no thread runs. */
	struct publish_shared *shared;
	pthread_t thread;
};

/*
 * Starts the thread that publishes the status object as cake-adapt.<section>
 * whenever ubus is reachable. Needs uloop_init() first; see publish.c.
 */
void publish_start(struct monitor *monitor, const char *section_name);

/*
 * Hands the thread the current status and logs what it reported since; ends
 * the run when ubusd refused the object on the first connection.
 */
void publish_update(struct monitor *monitor);

/* Stops the thread, waiting a bounded time; safe when publish_start never ran. */
void publish_stop(struct monitor *monitor);

#endif
