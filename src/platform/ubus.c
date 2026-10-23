#define _GNU_SOURCE

#include "platform/ubus.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "constants/ubus.h"

#include <libubus.h>
#include <libubox/utils.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* Whether the thread should keep running; the zeroed state is running. */
enum server_run {
	SERVER_RUN,
	SERVER_STOP
};

/* The thread's connection to ubusd. */
enum server_connection {
	SERVER_OFFLINE,
	SERVER_ONLINE,
	/* The object was refused; no further attempt is made. */
	SERVER_ABANDONED
};

/*
 * State shared by the caller's thread and the ubus thread. The snapshot is
 * guarded by lock; run, events and both statuses are atomic; and the identity
 * fields are written once before the thread starts. The rest belongs to the
 * thread.
 *
 * It lives on the heap: a thread stuck inside libubus at shutdown is detached
 * and may still read it, so it is then deliberately leaked as the process
 * exits.
 */
struct ubus_server {
	pthread_t thread;
	pthread_mutex_t lock;
	void *snapshot;
	/* The thread's copy, taken under the lock for one reply. */
	void *reply_snapshot;
	size_t snapshot_size;
	void *reply_context;
	ubus_server_reply_fn *reply_fn;
	atomic_int run;
	/* enum ubus_server_event bits not yet taken. */
	atomic_uint events;
	atomic_int reconnect_status;
	atomic_int add_status;
	/* Wakes the thread's poll when run becomes SERVER_STOP. */
	int wake_fd;
	char object_name[UBUS_OBJECT_NAME_SIZE];
	/* Embedded, so the lost-connection callback finds the rest. */
	struct ubus_context context;
	enum server_connection connection;
	/* Set by libubus's lost-connection callback, cleared on the reconnection. */
	bool lost;
	/* Whether a connection was ever attempted, so a refusal is at start. */
	bool attempted;
	bool unconnected_reported;
	struct ubus_object object;
	struct ubus_object_type type;
	struct ubus_method methods[1];
	struct blob_buf reply;
};

/* Runs on the ubus thread. */
static int server_request(
	struct ubus_context *context,
	struct ubus_object *object,
	struct ubus_request_data *request,
	const char *method,
	struct blob_attr *message
)
{
	struct ubus_server *server = __extension__ container_of(object, struct ubus_server, object);
	struct blob_buf *reply = &server->reply;

	(void)method;
	(void)message;
	(void)pthread_mutex_lock(&server->lock);
	memcpy(server->reply_snapshot, server->snapshot, server->snapshot_size);
	(void)pthread_mutex_unlock(&server->lock);
	server->reply_fn(reply, server->reply_snapshot, server->reply_context);
	return ubus_send_reply(context, request, reply->head);
}

static bool server_stopping(struct ubus_server *server)
{
	return atomic_load(&server->run) == SERVER_STOP;
}

static void server_raise(struct ubus_server *server, enum ubus_server_event event)
{
	atomic_fetch_or(&server->events, (unsigned int)event);
}

/*
 * libubus calls this from ubus_handle_event() and from inside its own
 * requests when the socket reaches its end. It only marks the loss: the thread
 * reconnects once libubus has returned, not from inside a request in progress.
 */
static void server_connection_lost(struct ubus_context *context)
{
	struct ubus_server *server =
		__extension__ container_of(context, struct ubus_server, context);

	server->lost = true;
}

static void server_close(struct ubus_server *server, enum server_connection connection)
{
	ubus_shutdown(&server->context);
	server->connection = connection;
	server->lost = false;
}

/*
 * Adds the object, with both IDs cleared: they belong to the previous ubusd,
 * which may have been restarted. A refusal ends the attempts, as in dnsmasq.
 */
static bool server_add(struct ubus_server *server, enum ubus_server_event refusal)
{
	struct ubus_object *object = &server->object;
	struct ubus_object_type *type = &server->type;
	int status;

	object->id = 0U;
	type->id = 0U;
	if ((status = ubus_add_object(&server->context, object)) == 0)
		return true;
	atomic_store(&server->add_status, status);
	server_raise(server, refusal);
	server_close(server, SERVER_ABANDONED);
	return false;
}

/*
 * Connects and adds the object. A failed connection is reported once until a
 * connection is made; it is attempted again after a wait.
 */
static void server_connect(struct ubus_server *server)
{
	const bool at_start = !server->attempted;
	struct ubus_context *context = &server->context;

	server->attempted = true;
	if (ubus_connect_ctx(context, NULL) != 0) {
		if (!server->unconnected_reported)
			server_raise(server, UBUS_SERVER_NO_CONNECTION);
		server->unconnected_reported = true;
		return;
	}
	server->connection = SERVER_ONLINE;
	server->lost = false;
	context->connection_lost = server_connection_lost;
	if (!server_add(server, at_start ? UBUS_SERVER_ADD_FAILED_AT_START : UBUS_SERVER_ADD_FAILED))
		return;
	/* As dnsmasq, only the connection made by the very first attempt is not logged. */
	if (!at_start)
		server_raise(server, UBUS_SERVER_CONNECTED);
	server->unconnected_reported = false;
}

/*
 * After a lost connection, as dnsmasq: reconnect at once, which also adds the
 * object again, or close and leave the next attempt to server_connect().
 */
static void server_reconnect(struct ubus_server *server)
{
	const struct ubus_object *object = &server->object;
	int status;

	server->lost = false;
	if ((status = ubus_reconnect(&server->context, NULL)) == 0 && server->lost)
		status = UBUS_STATUS_CONNECTION_FAILED;
	if (status != 0) {
		atomic_store(&server->reconnect_status, status);
		server_raise(server, UBUS_SERVER_RECONNECT_FAILED);
		server_close(server, SERVER_OFFLINE);
		return;
	}
	/* libubus ignores a refused object here; adding it once more gives the reason. */
	if (object->id == 0U)
		(void)server_add(server, UBUS_SERVER_ADD_FAILED);
}

/*
 * Waits up to UBUS_SERVER_WAIT_US for a stop or, when connected, for ubus
 * data, and handles that data. A hang-up libubus did not see as the end of the
 * connection closes it, as in dnsmasq.
 */
static void server_wait(struct ubus_server *server)
{
	const bool online = server->connection == SERVER_ONLINE;
	struct ubus_context *context = &server->context;
	const struct uloop_fd *sock = &context->sock;
	/* poll() ignores a negative descriptor, and never reports it. */
	struct pollfd descriptors[] = {
		{ .fd = server->wake_fd, .events = POLLIN },
		{ .fd = online ? sock->fd : -1, .events = POLLIN },
	};
	const int wait_ms = us_to_timeout_ms(UBUS_SERVER_WAIT_US);

	if (poll(descriptors, (nfds_t)ARRAY_SIZE(descriptors), wait_ms) <= 0)
		return;
	if (descriptors[1].revents & POLLIN)
		ubus_handle_event(context);
	if (server->lost || !(descriptors[1].revents & (POLLHUP | POLLERR)))
		return;
	server_raise(server, UBUS_SERVER_DISCONNECTED);
	server_close(server, SERVER_OFFLINE);
}

/*
 * The ubus thread. Its context is never added to uloop, so libubus arms no
 * uloop timer or descriptor of the main thread, and every libubus call that
 * can block forever on a stalled ubusd blocks only this thread.
 */
static void *server_thread(void *argument)
{
	struct ubus_server *server = argument;

	while (!server_stopping(server)) {
		if (server->connection == SERVER_OFFLINE)
			server_connect(server);
		server_wait(server);
		if (server->lost && !server_stopping(server))
			server_reconnect(server);
	}
	if (server->connection == SERVER_ONLINE)
		server_close(server, SERVER_OFFLINE);
	blob_buf_free(&server->reply);
	return NULL;
}

static void server_free(struct ubus_server *server)
{
	if (server->wake_fd >= 0)
		(void)close(server->wake_fd);
	(void)pthread_mutex_destroy(&server->lock);
	free(server->snapshot);
	free(server->reply_snapshot);
	free(server->reply_context);
	free(server);
}

/* Starts the thread with every signal blocked, so uloop handles them in the main thread. */
static int server_spawn(struct ubus_server *server)
{
	sigset_t all;
	sigset_t previous;
	int error;

	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	error = pthread_create(&server->thread, NULL, server_thread, server);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	return error;
}

struct ubus_server *ubus_server_start(const struct ubus_server_settings *settings, int *error)
{
	struct ubus_server *server = calloc(1U, sizeof(*server));

	if (server == NULL) {
		*error = errno;
		return NULL;
	}
	/* Before anything takes the lock, and before server_free may destroy it. */
	(void)pthread_mutex_init(&server->lock, NULL);
	server->wake_fd = -1;
	server->snapshot_size = settings->snapshot_size;
	server->snapshot = malloc(settings->snapshot_size);
	server->reply_snapshot = malloc(settings->snapshot_size);
	server->reply_context = malloc(settings->context_size);
	if (server->snapshot == NULL || server->reply_snapshot == NULL ||
	    server->reply_context == NULL) {
		*error = errno;
		server_free(server);
		return NULL;
	}
	memcpy(server->snapshot, settings->snapshot, settings->snapshot_size);
	memcpy(server->reply_context, settings->context, settings->context_size);
	server->reply_fn = settings->reply;
	(
		void
	)snprintf(server->object_name, sizeof(server->object_name), "%s", settings->object_name);
	server->methods[0] =
		(struct ubus_method)UBUS_METHOD_NOARG(settings->method, server_request);
	server->type = (struct ubus_object_type){
		.name = settings->type_name,
		.methods = server->methods,
		.n_methods = ARRAY_SIZE(server->methods),
	};
	server->object = (struct ubus_object){
		.name = server->object_name,
		.type = &server->type,
		.methods = server->methods,
		.n_methods = ARRAY_SIZE(server->methods),
	};
	atomic_init(&server->run, SERVER_RUN);
	atomic_init(&server->events, 0U);
	atomic_init(&server->reconnect_status, 0);
	atomic_init(&server->add_status, 0);
	server->wake_fd = eventfd(0U, EFD_CLOEXEC);
	if (server->wake_fd < 0) {
		*error = errno;
		server_free(server);
		return NULL;
	}
	if ((*error = server_spawn(server)) != 0) {
		server_free(server);
		return NULL;
	}
	return server;
}

void ubus_server_update(struct ubus_server *server, const void *snapshot)
{
	(void)pthread_mutex_lock(&server->lock);
	memcpy(server->snapshot, snapshot, server->snapshot_size);
	(void)pthread_mutex_unlock(&server->lock);
}

unsigned int ubus_server_take_events(struct ubus_server *server)
{
	return atomic_exchange(&server->events, 0U);
}

int ubus_server_reconnect_status(const struct ubus_server *server)
{
	return atomic_load(&server->reconnect_status);
}

int ubus_server_add_status(const struct ubus_server *server)
{
	return atomic_load(&server->add_status);
}

const char *ubus_server_object_name(const struct ubus_server *server)
{
	return server->object_name;
}

bool ubus_server_stop(struct ubus_server *server, uint64_t within_us)
{
	atomic_store(&server->run, SERVER_STOP);
	/* Without the wake, the thread still sees the stop within UBUS_SERVER_WAIT_US. */
	(void)eventfd_write(server->wake_fd, 1U);
	if (thread_join_within(server->thread, within_us)) {
		server_free(server);
		return true;
	}
	/* Stuck inside libubus on a stalled ubusd: the process is exiting anyway. */
	(void)pthread_detach(server->thread);
	return false;
}
