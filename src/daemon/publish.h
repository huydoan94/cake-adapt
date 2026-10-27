#ifndef DAEMON_PUBLISH_H_INCLUDED
#define DAEMON_PUBLISH_H_INCLUDED

/*
 * publish.c: the status object on ubus, its reply and what its server
 * reports. The server (platform/ubus.h) serves it from its own thread, so the
 * main thread never calls libubus beyond ubus_strerror().
 */

struct daemon;

struct ubus_server;

struct daemon_publish {
	/* NULL while no thread runs. */
	struct ubus_server *server;
};

/*
 * Starts the thread that publishes the status object as cake-adapt.<section>
 * whenever ubus is reachable. Needs uloop_init() first; see platform/ubus.h.
 */
void publish_start(struct daemon *daemon, const char *section_name);

/*
 * Hands the thread the current status and logs what it reported since; ends
 * the run when ubusd refused the object on the first connection.
 */
void publish_update(struct daemon *daemon);

/* Stops the thread, waiting a bounded time; safe when publish_start never ran. */
void publish_stop(struct daemon *daemon);

#endif
