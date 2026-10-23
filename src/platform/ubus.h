#ifndef PLATFORM_UBUS_H_INCLUDED
#define PLATFORM_UBUS_H_INCLUDED

/*
 * One object on ubus, served by a thread of its own from a snapshot the
 * caller replaces under a lock: a stalled ubusd blocks only that thread, and
 * the caller never calls libubus beyond ubus_strerror(). The thread connects,
 * adds the object, answers its one method, and reconnects as dnsmasq does; it
 * reports what happened as events for the caller to log.
 */

#include <libubox/blobmsg.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the thread reports, as bits the caller takes with ubus_server_take_events(). */
enum ubus_server_event {
	/* POLLHUP or POLLERR on the socket: closed, retried. */
	UBUS_SERVER_DISCONNECTED = 1U << 0,
	/* A lost connection that could not be made again at once: closed, retried. */
	UBUS_SERVER_RECONNECT_FAILED = 1U << 1,
	/* No connection to ubusd, reported once until one is made. */
	UBUS_SERVER_NO_CONNECTION = 1U << 2,
	/*
	 * Connected and published by any attempt but the first: after no ubusd, a
	 * failed reconnection or a hang-up.
	 */
	UBUS_SERVER_CONNECTED = 1U << 3,
	/* ubusd refused the object on a later connection: the thread stops trying. */
	UBUS_SERVER_ADD_FAILED = 1U << 4,
	/* ubusd refused the object on the first connection. */
	UBUS_SERVER_ADD_FAILED_AT_START = 1U << 5,
};

/* Fills reply from a copy of the snapshot; runs on the server's thread. */
typedef void
ubus_server_reply_fn(struct blob_buf *reply, const void *snapshot, const void *context);

struct ubus_server_settings {
	/* Copied, and cut to UBUS_OBJECT_NAME_SIZE. */
	const char *object_name;
	/* Kept as given: both must outlive the server, as string literals do. */
	const char *type_name;
	const char *method;
	/*
	 * The first snapshot, copied; later ones come through ubus_server_update().
	 * Both sizes are above zero.
	 */
	const void *snapshot;
	size_t snapshot_size;
	ubus_server_reply_fn *reply;
	/* Copied once, for the reply: what does not change while the object is served. */
	const void *context;
	size_t context_size;
};

struct ubus_server;

/*
 * Starts the thread with every signal blocked. NULL on failure, with the
 * errno value in *error. ubus_connect() calls uloop_init() from the thread;
 * that is harmless only when uloop is already initialized in the main thread.
 */
struct ubus_server *ubus_server_start(const struct ubus_server_settings *settings, int *error);

/* Replaces the snapshot the thread answers from. */
void ubus_server_update(struct ubus_server *server, const void *snapshot);

/* The enum ubus_server_event bits reported since the last call. */
unsigned int ubus_server_take_events(struct ubus_server *server);

/* The ubus status behind UBUS_SERVER_RECONNECT_FAILED, and behind the add failures. */
int ubus_server_reconnect_status(const struct ubus_server *server);
int ubus_server_add_status(const struct ubus_server *server);

const char *ubus_server_object_name(const struct ubus_server *server);

/*
 * Stops the thread, waiting up to within_us. False when it is stuck inside
 * libubus on a stalled ubusd: it is then detached and left behind with its
 * state, as the process is exiting anyway.
 */
bool ubus_server_stop(struct ubus_server *server, uint64_t within_us);

#endif
