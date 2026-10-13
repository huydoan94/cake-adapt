#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/utils.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define FIELD_VERSION "version"
#define FIELD_PID "pid"
#define FIELD_STARTED "started"
#define FIELD_STATE "state"
#define FIELD_DOWNLOAD "download"
#define FIELD_UPLOAD "upload"
#define FIELD_INTERFACE "interface"
#define FIELD_AVAILABLE "available"
#define FIELD_SHAPER "shaper_bps"
#define FIELD_ACHIEVED "achieved_bps"
#define FIELD_LOAD "load_e6"
#define FIELD_DELAY "delay_us"
#define FIELD_BUFFERBLOAT "bufferbloat"
#define FIELD_TCP_QUEUE "tcp_queue_us"

#define PUBLISH_OBJECT_NAME_SIZE (sizeof(UBUS_OBJECT_PREFIX) + 1U + CONFIG_STRING_SIZE)

/* The thread's longest wait, both between connection attempts and for requests. */
#define PUBLISH_WAIT_US SECOND
/* How long shutdown waits for the thread before leaving it behind, checked this often. */
#define PUBLISH_STOP_US (2U * SECOND)

/* Whether the thread should keep running; the zeroed state is running. */
enum publish_run {
	PUBLISH_RUN,
	PUBLISH_STOP
};

/*
 * What the thread reports, as bits the main thread collects and logs on its
 * next traffic tick, with dnsmasq's message and level for the same ubus
 * event. The two info events go through log_system_message(), so they reach
 * syslog at info level, as dnsmasq logs them, and the daemon's log file.
 */
enum publish_event {
	/* POLLHUP or POLLERR on the socket: closed, retried. */
	PUBLISH_DISCONNECTED = 1U << 0,
	/* A lost connection that could not be made again at once: closed, retried. */
	PUBLISH_RECONNECT_FAILED = 1U << 1,
	/* No connection to ubusd, reported once until one is made. */
	PUBLISH_NO_CONNECTION = 1U << 2,
	/*
	 * Connected and published by any attempt but the first: after no ubusd, a
	 * failed reconnection or a hang-up.
	 */
	PUBLISH_CONNECTED = 1U << 3,
	/* ubusd refused the object on a later connection: the thread stops trying. */
	PUBLISH_ADD_FAILED = 1U << 4,
	/* ubusd refused the object on the first connection: the daemon exits. */
	PUBLISH_ADD_FAILED_AT_START = 1U << 5,
};

/* The thread's connection to ubusd. */
enum publish_connection {
	PUBLISH_OFFLINE,
	PUBLISH_ONLINE,
	/* The object was refused; no further attempt is made. */
	PUBLISH_ABANDONED
};

/*
 * State shared by the main thread and the ubus thread. The snapshot is guarded
 * by lock; run, events and both statuses are atomic; and the identity fields
 * are written once before the thread starts. The rest belongs to the thread.
 *
 * It lives on the heap, not in monitor_run's frame: a thread stuck inside
 * libubus at shutdown is detached and may still read it, so it is then
 * deliberately leaked as the process exits.
 */
struct publish_shared {
	pthread_mutex_t lock;
	struct status_snapshot snapshot;
	atomic_int run;
	/* enum publish_event bits not yet logged. */
	atomic_uint events;
	/* The ubus status behind PUBLISH_RECONNECT_FAILED and the add failures. */
	atomic_int reconnect_status;
	atomic_int add_status;
	/* Wakes the thread's poll when run becomes PUBLISH_STOP. */
	int wake_fd;
	char object_name[PUBLISH_OBJECT_NAME_SIZE];
	char download_interface[IF_NAMESIZE];
	char upload_interface[IF_NAMESIZE];
	uint32_t pid;
	uint64_t started_sec;
	/* Embedded, so the lost-connection callback finds the rest. */
	struct ubus_context context;
	enum publish_connection connection;
	/* Set by libubus's lost-connection callback, cleared on the reconnection. */
	bool lost;
	/* Whether a connection was ever attempted, so a refusal is at start. */
	bool attempted;
	bool unconnected_reported;
	struct ubus_object object;
	struct ubus_object_type type;
	struct blob_buf reply;
};

static const char *publish_state_name(enum controller_activity_state state)
{
	switch (state) {
	case CONTROLLER_IDLE:
		return STATE_IDLE;
	case CONTROLLER_STALL:
		return STATE_STALL;
	default:
		return STATE_RUNNING;
	}
}

static void
publish_direction(struct blob_buf *reply, const char *name, const struct status_direction *direction)
{
	void *table = blobmsg_open_table(reply, name);

	blobmsg_add_string(reply, FIELD_INTERFACE, direction->interface);
	blobmsg_add_u8(reply, FIELD_AVAILABLE, direction->available);
	if (direction->available) {
		blobmsg_add_u64(reply, FIELD_SHAPER, direction->shaper_bps);
		blobmsg_add_u64(reply, FIELD_ACHIEVED, direction->achieved_bps);
		blobmsg_add_u64(reply, FIELD_LOAD, direction->load_e6);
		/* blobmsg prints INT64 as signed, so a negative delay survives. */
		blobmsg_add_u64(reply, FIELD_DELAY, (uint64_t)direction->delay_us);
		blobmsg_add_u8(reply, FIELD_BUFFERBLOAT, direction->bufferbloat);
		if (direction->tcp_queue_valid)
			blobmsg_add_u64(reply, FIELD_TCP_QUEUE, (uint64_t)direction->tcp_queue_us);
	}
	blobmsg_close_table(reply, table);
}

/* The status reply; the snapshot's interface pointers are not used. */
static void publish_reply(
	struct blob_buf *reply,
	const struct publish_shared *shared,
	const struct status_snapshot *snapshot
)
{
	struct status_direction download = snapshot->download;
	struct status_direction upload = snapshot->upload;

	download.interface = shared->download_interface;
	upload.interface = shared->upload_interface;
	blob_buf_init(reply, 0);
	blobmsg_add_string(reply, FIELD_VERSION, PROGRAM_VERSION);
	blobmsg_add_u32(reply, FIELD_PID, shared->pid);
	blobmsg_add_u64(reply, FIELD_STARTED, shared->started_sec);
	blobmsg_add_string(reply, FIELD_STATE, publish_state_name(snapshot->state));
	publish_direction(reply, FIELD_DOWNLOAD, &download);
	publish_direction(reply, FIELD_UPLOAD, &upload);
}

/* Runs on the ubus thread. */
static int publish_status(
	struct ubus_context *context,
	struct ubus_object *object,
	struct ubus_request_data *request,
	const char *method,
	struct blob_attr *message
)
{
	struct publish_shared *shared =
		__extension__ container_of(object, struct publish_shared, object);
	struct blob_buf *reply = &shared->reply;
	struct status_snapshot snapshot;

	(void)method;
	(void)message;
	(void)pthread_mutex_lock(&shared->lock);
	snapshot = shared->snapshot;
	(void)pthread_mutex_unlock(&shared->lock);
	publish_reply(reply, shared, &snapshot);
	return ubus_send_reply(context, request, reply->head);
}

static const struct ubus_method publish_methods[] = {
	UBUS_METHOD_NOARG(UBUS_METHOD_STATUS, publish_status),
};

static bool publish_stopping(struct publish_shared *shared)
{
	return atomic_load(&shared->run) == PUBLISH_STOP;
}

static void publish_raise(struct publish_shared *shared, enum publish_event event)
{
	atomic_fetch_or(&shared->events, (unsigned int)event);
}

/*
 * libubus calls this from ubus_handle_event() and from inside its own
 * requests when the socket reaches its end. It only marks the loss: the thread
 * reconnects once libubus has returned, not from inside a request in progress.
 */
static void publish_connection_lost(struct ubus_context *context)
{
	struct publish_shared *shared =
		__extension__ container_of(context, struct publish_shared, context);

	shared->lost = true;
}

static void publish_close(struct publish_shared *shared, enum publish_connection connection)
{
	ubus_shutdown(&shared->context);
	shared->connection = connection;
	shared->lost = false;
}

/*
 * Adds the object, with both IDs cleared: they belong to the previous ubusd,
 * which may have been restarted. A refusal ends the attempts, as in dnsmasq.
 */
static bool publish_add(struct publish_shared *shared, enum publish_event refusal)
{
	struct ubus_object *object = &shared->object;
	struct ubus_object_type *type = &shared->type;
	int status;

	object->id = 0U;
	type->id = 0U;
	if ((status = ubus_add_object(&shared->context, object)) == 0)
		return true;
	atomic_store(&shared->add_status, status);
	publish_raise(shared, refusal);
	publish_close(shared, PUBLISH_ABANDONED);
	return false;
}

/*
 * Connects and adds the object. A failed connection is reported once until a
 * connection is made; it is attempted again after a wait.
 */
static void publish_connect(struct publish_shared *shared)
{
	const bool at_start = !shared->attempted;
	struct ubus_context *context = &shared->context;

	shared->attempted = true;
	if (ubus_connect_ctx(context, NULL) != 0) {
		if (!shared->unconnected_reported)
			publish_raise(shared, PUBLISH_NO_CONNECTION);
		shared->unconnected_reported = true;
		return;
	}
	shared->connection = PUBLISH_ONLINE;
	shared->lost = false;
	context->connection_lost = publish_connection_lost;
	if (!publish_add(shared, at_start ? PUBLISH_ADD_FAILED_AT_START : PUBLISH_ADD_FAILED))
		return;
	/* As dnsmasq, only the connection made by the very first attempt is not logged. */
	if (!at_start)
		publish_raise(shared, PUBLISH_CONNECTED);
	shared->unconnected_reported = false;
}

/*
 * After a lost connection, as dnsmasq: reconnect at once, which also adds the
 * object again, or close and leave the next attempt to publish_connect().
 */
static void publish_reconnect(struct publish_shared *shared)
{
	const struct ubus_object *object = &shared->object;
	int status;

	shared->lost = false;
	if ((status = ubus_reconnect(&shared->context, NULL)) == 0 && shared->lost)
		status = UBUS_STATUS_CONNECTION_FAILED;
	if (status != 0) {
		atomic_store(&shared->reconnect_status, status);
		publish_raise(shared, PUBLISH_RECONNECT_FAILED);
		publish_close(shared, PUBLISH_OFFLINE);
		return;
	}
	/* libubus ignores a refused object here; adding it once more gives the reason. */
	if (object->id == 0U)
		(void)publish_add(shared, PUBLISH_ADD_FAILED);
}

/*
 * Waits up to PUBLISH_WAIT_US for a stop or, when connected, for ubus data,
 * and handles that data. A hang-up libubus did not see as the end of the
 * connection closes it, as in dnsmasq.
 */
static void publish_wait(struct publish_shared *shared)
{
	const bool online = shared->connection == PUBLISH_ONLINE;
	struct ubus_context *context = &shared->context;
	const struct uloop_fd *sock = &context->sock;
	/* poll() ignores a negative descriptor, and never reports it. */
	struct pollfd descriptors[] = {
		{ .fd = shared->wake_fd, .events = POLLIN },
		{ .fd = online ? sock->fd : -1, .events = POLLIN },
	};
	const int wait_ms = (int)us_to_ms(PUBLISH_WAIT_US);

	if (poll(descriptors, (nfds_t)ARRAY_SIZE(descriptors), wait_ms) <= 0)
		return;
	if (descriptors[1].revents & POLLIN)
		ubus_handle_event(context);
	if (shared->lost || !(descriptors[1].revents & (POLLHUP | POLLERR)))
		return;
	publish_raise(shared, PUBLISH_DISCONNECTED);
	publish_close(shared, PUBLISH_OFFLINE);
}

/*
 * The ubus thread. Its context is never added to uloop, so libubus arms no
 * uloop timer or descriptor of the main thread, and every libubus call that
 * can block forever on a stalled ubusd blocks only this thread.
 */
static void *publish_thread(void *argument)
{
	struct publish_shared *shared = argument;

	while (!publish_stopping(shared)) {
		if (shared->connection == PUBLISH_OFFLINE)
			publish_connect(shared);
		publish_wait(shared);
		if (shared->lost && !publish_stopping(shared))
			publish_reconnect(shared);
	}
	if (shared->connection == PUBLISH_ONLINE)
		publish_close(shared, PUBLISH_OFFLINE);
	blob_buf_free(&shared->reply);
	return NULL;
}

static void publish_copy(struct monitor *monitor, struct publish_shared *shared, uint64_t now_us)
{
	struct status_snapshot snapshot;

	status_snapshot(monitor, now_us, &snapshot);
	(void)pthread_mutex_lock(&shared->lock);
	shared->snapshot = snapshot;
	(void)pthread_mutex_unlock(&shared->lock);
}

/*
 * Logs what the thread reported since the last tick, in the order the events
 * follow each other, with dnsmasq's levels. A refusal on the first connection
 * ends the run, as dnsmasq exits then; procd starts the daemon again.
 */
static void publish_report(struct monitor *monitor, unsigned int events)
{
	const struct monitor_publish *publish = &monitor->publish;
	struct publish_shared *shared = publish->shared;

	if (events & PUBLISH_DISCONNECTED)
		log_system_message("disconnecting from ubus");
	if (events & PUBLISH_RECONNECT_FAILED)
		log_message(
			LOG_LEVEL_ERROR,
			"cannot reconnect to ubus: %s",
			ubus_strerror(atomic_load(&shared->reconnect_status))
		);
	if (events & PUBLISH_NO_CONNECTION)
		log_message(LOG_LEVEL_ERROR, "cannot publish status on ubus: no connection");
	if (events & PUBLISH_CONNECTED)
		log_system_message("connected to ubus; status published as %s", shared->object_name);
	if (events & PUBLISH_ADD_FAILED)
		log_message(
			LOG_LEVEL_ERROR,
			"ubus error: %s; status not published",
			ubus_strerror(atomic_load(&shared->add_status))
		);
	if (events & PUBLISH_ADD_FAILED_AT_START) {
		log_message(
			LOG_LEVEL_ERROR,
			"ubus error: %s; exiting",
			ubus_strerror(atomic_load(&shared->add_status))
		);
		monitor->result = -1;
		uloop_end();
	}
}

static void publish_free(struct publish_shared *shared)
{
	if (shared->wake_fd >= 0)
		(void)close(shared->wake_fd);
	(void)pthread_mutex_destroy(&shared->lock);
	free(shared);
}

/* Starts the thread with every signal blocked, so uloop handles them in the main thread. */
static int publish_spawn(struct monitor_publish *publish, struct publish_shared *shared)
{
	sigset_t all;
	sigset_t previous;
	int error;

	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	error = pthread_create(&publish->thread, NULL, publish_thread, shared);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	return error;
}

/*
 * ubus_connect() calls uloop_init() from the thread; that is harmless only
 * because monitor_run() has already initialized uloop in the main thread, so
 * the call finds everything in place and changes nothing.
 */
void publish_start(struct monitor *monitor, const char *section_name)
{
	struct monitor_publish *publish = &monitor->publish;
	const struct monitor_links *links = &monitor->links;
	const struct monitor_direction *download = &links->download;
	const struct monitor_direction *upload = &links->upload;
	struct publish_shared *shared = calloc(1U, sizeof(*shared));
	uint64_t now_us;
	int error;

	if (shared == NULL) {
		log_message(LOG_LEVEL_WARNING, "status not published on ubus: %s", strerror(errno));
		return;
	}
	(void)snprintf(
		shared->object_name,
		sizeof(shared->object_name),
		"%s.%s",
		UBUS_OBJECT_PREFIX,
		section_name
	);
	(void)snprintf(
		shared->download_interface,
		sizeof(shared->download_interface),
		"%s",
		download->interface
	);
	(void)snprintf(
		shared->upload_interface,
		sizeof(shared->upload_interface),
		"%s",
		upload->interface
	);
	shared->pid = (uint32_t)getpid();
	if (read_clock_us(CLOCK_REALTIME, &now_us))
		shared->started_sec = now_us / MICROSECONDS_PER_SECOND;
	shared->type =
		(struct ubus_object_type)UBUS_OBJECT_TYPE(UBUS_OBJECT_PREFIX, publish_methods);
	shared->object = (struct ubus_object){
		.name = shared->object_name,
		.type = &shared->type,
		.methods = publish_methods,
		.n_methods = ARRAY_SIZE(publish_methods),
	};
	atomic_init(&shared->run, PUBLISH_RUN);
	atomic_init(&shared->events, 0U);
	atomic_init(&shared->reconnect_status, 0);
	atomic_init(&shared->add_status, 0);
	/* Before anything takes the lock, and before publish_free may destroy it. */
	(void)pthread_mutex_init(&shared->lock, NULL);
	shared->wake_fd = eventfd(0U, EFD_CLOEXEC);
	if (shared->wake_fd < 0) {
		log_message(LOG_LEVEL_WARNING, "status not published on ubus: %s", strerror(errno));
		publish_free(shared);
		return;
	}
	if (read_clock_us(CLOCK_MONOTONIC, &now_us))
		publish_copy(monitor, shared, now_us);
	if ((error = publish_spawn(publish, shared)) != 0) {
		log_message(LOG_LEVEL_WARNING, "status not published on ubus: %s", strerror(error));
		publish_free(shared);
		return;
	}
	publish->shared = shared;
}

void publish_update(struct monitor *monitor)
{
	struct monitor_publish *publish = &monitor->publish;
	uint64_t now_us;

	if (publish->shared == NULL || !read_clock_us(CLOCK_MONOTONIC, &now_us))
		return;
	publish_copy(monitor, publish->shared, now_us);
	publish_report(monitor, atomic_exchange(&publish->shared->events, 0U));
}

void publish_stop(struct monitor *monitor)
{
	struct monitor_publish *publish = &monitor->publish;
	struct publish_shared *shared = publish->shared;

	if (shared == NULL)
		return;
	publish->shared = NULL;
	atomic_store(&shared->run, PUBLISH_STOP);
	/* Without the wake, the thread still sees the stop within PUBLISH_WAIT_US. */
	(void)eventfd_write(shared->wake_fd, 1U);
	if (thread_join_within(publish->thread, PUBLISH_STOP_US)) {
		publish_free(shared);
		return;
	}
	/* Stuck inside libubus on a stalled ubusd: the process is exiting anyway. */
	(void)pthread_detach(publish->thread);
	log_message(
		LOG_LEVEL_WARNING,
		"ubus status thread did not stop within %.1f s; leaving it behind",
		us_to_sec_decimal(PUBLISH_STOP_US)
	);
}
