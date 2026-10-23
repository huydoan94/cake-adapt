/*
 * publish.c with libubus and blobmsg replaced by recorders: the reply built from
 * a snapshot, the thread's start, request and bounded stop, and every ubus state
 * dnsmasq handles, with the messages and levels the main thread logs for it.
 */
#include "monitor/publish.c"
#include "constants/program.h"
#include "constants/ubus.h"
#include "constants/units.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>

#define WAIT_LIMIT_US (3U * SECOND)
#define WAIT_STEP_US (5U * MILLISECOND)

/* Where a message went: log_message(), or log_system_message() for syslog at info. */
enum message_route {
	ROUTE_LOG,
	ROUTE_SYSTEM
};

/* Every logged message, its level and route, in order. */
static char messages[8][256];
static enum log_level levels[8];
static enum message_route routes[8];
static size_t message_count;

/* Set by uloop_end(), which publish.c calls to end the run. */
bool uloop_cancelled;

static void
record_message(enum log_level level, enum message_route route, const char *format, va_list arguments)
{
	assert(message_count < ARRAY_SIZE(messages));
	levels[message_count] = level;
	routes[message_count] = route;
	(void)vsnprintf(messages[message_count], sizeof(messages[0]), format, arguments);
	message_count++;
}

void log_message(enum log_level level, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	record_message(level, ROUTE_LOG, format, arguments);
	va_end(arguments);
}

void log_system_message(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	record_message(LOG_LEVEL_INFO, ROUTE_SYSTEM, format, arguments);
	va_end(arguments);
}

static struct status_snapshot supplied_snapshot;

void status_snapshot(
	struct monitor *monitor,
	uint64_t timestamp_us,
	struct status_snapshot *snapshot
)
{
	(void)monitor;
	(void)timestamp_us;
	*snapshot = supplied_snapshot;
}

/* The blobmsg calls of one reply, as name=value strings and table brackets. */
static char fields[32][64];
static size_t field_count;

int blob_buf_init(struct blob_buf *buf, int id)
{
	(void)buf;
	(void)id;
	field_count = 0U;
	return 0;
}

/* Set when blob_buf_free() runs, the ubus thread's last call. */
static atomic_bool thread_finished;

void blob_buf_free(struct blob_buf *buf)
{
	(void)buf;
	atomic_store(&thread_finished, true);
}

int blobmsg_add_field(
	struct blob_buf *buf,
	int type,
	const char *name,
	const void *data,
	unsigned int len
)
{
	uint64_t value = 0U;
	char *field;

	(void)buf;
	(void)len;
	assert(field_count < ARRAY_SIZE(fields));
	field = fields[field_count];
	switch (type) {
	case BLOBMSG_TYPE_STRING:
		(void)snprintf(field, sizeof(fields[0]), "%s=%s", name, (const char *)data);
		break;
	case BLOBMSG_TYPE_INT8:
		value = *(const uint8_t *)data;
		break;
	case BLOBMSG_TYPE_INT32:
		value = be32_to_cpu(*(const uint32_t *)data);
		break;
	case BLOBMSG_TYPE_INT64:
		value = be64_to_cpu(*(const uint64_t *)data);
		break;
	default:
		assert(0);
	}
	if (type != BLOBMSG_TYPE_STRING)
		(void)snprintf(field, sizeof(fields[0]), "%s=%lld", name, (long long)value);
	field_count++;
	return 0;
}

void *blobmsg_open_nested(struct blob_buf *buf, const char *name, bool array)
{
	(void)buf;
	(void)array;
	(void)snprintf(fields[field_count++], sizeof(fields[0]), "%s{", name);
	return NULL;
}

void blob_nest_end(struct blob_buf *buf, void *cookie)
{
	(void)buf;
	(void)cookie;
	(void)snprintf(fields[field_count++], sizeof(fields[0]), "}");
}

/*
 * A fake ubusd: connecting hands out one end of a socket pair. A byte the test
 * writes to the other end is a status request; closing it ends the connection,
 * which, as in libubus, calls the context's connection_lost callback.
 */
static atomic_bool ubusd_running;
static atomic_int add_result;
static atomic_int connects;
static atomic_int reconnects;
static atomic_int adds;
static atomic_int replies;
static atomic_uint object_id_before_add;
static atomic_uint type_id_before_add;
static atomic_int ubusd_fd = -1;
/* While set, connecting blocks, as on a frozen ubusd. */
static atomic_bool connect_hangs;
/* While set, the end of the connection goes unnoticed, leaving only POLLHUP. */
static atomic_bool end_unnoticed;
static struct ubus_object *added_object;

static void fake_socket_data(struct uloop_fd *socket, unsigned int events)
{
	struct ubus_context *context =
		__extension__ container_of(socket, struct ubus_context, sock);
	char request;

	(void)events;
	if (read(socket->fd, &request, sizeof(request)) > 0) {
		(void)added_object->methods[0]
			.handler(context, added_object, NULL, UBUS_METHOD_STATUS, NULL);
		return;
	}
	if (atomic_load(&end_unnoticed))
		return;
	socket->eof = true;
	context->connection_lost(context);
}

static int fake_add(struct ubus_object *object)
{
	atomic_store(&object_id_before_add, object->id);
	atomic_store(&type_id_before_add, object->type->id);
	added_object = object;
	if (atomic_load(&add_result) != 0)
		return atomic_load(&add_result);
	object->id = 7U;
	object->type->id = 9U;
	atomic_fetch_add(&adds, 1);
	return 0;
}

static bool fake_socket(struct ubus_context *context)
{
	struct uloop_fd *sock = &context->sock;
	int pair[2];

	if (!atomic_load(&ubusd_running))
		return false;
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	sock->fd = pair[0];
	atomic_store(&ubusd_fd, pair[1]);
	return true;
}

int ubus_connect_ctx(struct ubus_context *context, const char *path)
{
	assert(path == NULL);
	atomic_fetch_add(&connects, 1);
	while (atomic_load(&connect_hangs)) {
		const struct timespec step = us_to_timespec(MILLISECOND);

		(void)nanosleep(&step, NULL);
	}
	*context = (struct ubus_context){ .sock = { .fd = -1, .cb = fake_socket_data } };
	return fake_socket(context) ? 0 : -1;
}

/* As libubus: a new socket, then the object added again, its result ignored. */
int ubus_reconnect(struct ubus_context *context, const char *path)
{
	struct uloop_fd *sock = &context->sock;

	assert(path == NULL);
	atomic_fetch_add(&reconnects, 1);
	(void)close(sock->fd);
	sock->fd = -1;
	sock->eof = false;
	if (!fake_socket(context))
		return UBUS_STATUS_CONNECTION_FAILED;
	added_object->id = 0U;
	added_object->type->id = 0U;
	(void)fake_add(added_object);
	return 0;
}

int ubus_add_object(struct ubus_context *context, struct ubus_object *object)
{
	(void)context;
	return fake_add(object);
}

void ubus_shutdown(struct ubus_context *context)
{
	struct uloop_fd *sock = &context->sock;

	if (sock->fd >= 0)
		(void)close(sock->fd);
	sock->fd = -1;
}

const char *ubus_strerror(int error)
{
	switch (error) {
	case UBUS_STATUS_PERMISSION_DENIED:
		return "Permission denied";
	case UBUS_STATUS_CONNECTION_FAILED:
		return "Connection failed";
	default:
		return "Unknown error";
	}
}

int ubus_send_reply(
	struct ubus_context *context,
	struct ubus_request_data *request,
	struct blob_attr *message
)
{
	(void)context;
	(void)request;
	(void)message;
	atomic_fetch_add(&replies, 1);
	return 0;
}

static void pause_step(void)
{
	const struct timespec step = us_to_timespec(WAIT_STEP_US);

	(void)nanosleep(&step, NULL);
}

static void wait_for_count(atomic_int *counter, int count)
{
	uint64_t waited_us;

	for (waited_us = 0U; waited_us < WAIT_LIMIT_US; waited_us += WAIT_STEP_US) {
		if (atomic_load(counter) >= count)
			return;
		pause_step();
	}
	assert(0);
}

/* Waits until the thread has reported all of events, without taking them. */
static void wait_for_events(struct monitor *monitor, unsigned int events)
{
	const struct monitor_publish *publish = &monitor->publish;
	struct publish_shared *shared = publish->shared;
	uint64_t waited_us;

	for (waited_us = 0U; waited_us < WAIT_LIMIT_US; waited_us += WAIT_STEP_US) {
		if ((atomic_load(&shared->events) & events) == events)
			return;
		pause_step();
	}
	assert(0);
}

/* Checks that the thread makes no connection attempt for longer than its retry wait. */
static void assert_no_attempt(void)
{
	const int before = atomic_load(&connects) + atomic_load(&reconnects);
	uint64_t waited_us;

	for (waited_us = 0U; waited_us < PUBLISH_WAIT_US + 300U * MILLISECOND;
	     waited_us += WAIT_STEP_US)
		pause_step();
	assert(atomic_load(&connects) + atomic_load(&reconnects) == before);
}

static void assert_message(size_t index, enum log_level level, const char *text)
{
	assert(index < message_count);
	assert(levels[index] == level);
	assert(routes[index] == ROUTE_LOG);
	assert(strcmp(messages[index], text) == 0);
}

/* A message that also reaches syslog at info level, as dnsmasq logs it. */
static void assert_system_message(size_t index, const char *text)
{
	assert(index < message_count);
	assert(routes[index] == ROUTE_SYSTEM);
	assert(strcmp(messages[index], text) == 0);
}

static void reset(struct monitor *monitor)
{
	struct monitor_links *links = &monitor->links;
	struct monitor_direction *download = &links->download;
	struct monitor_direction *upload = &links->upload;

	*monitor = (struct monitor){ 0 };
	download->interface = "ifb4wan";
	upload->interface = "wan";
	message_count = 0U;
	field_count = 0U;
	uloop_cancelled = false;
	atomic_store(&ubusd_running, false);
	atomic_store(&end_unnoticed, false);
	atomic_store(&add_result, 0);
	atomic_store(&connects, 0);
	atomic_store(&reconnects, 0);
	atomic_store(&adds, 0);
	atomic_store(&replies, 0);
	supplied_snapshot = (struct status_snapshot){ 0 };
}

/* Stops the thread and checks it was joined, not left behind, well inside the bound. */
static void stop_and_check(struct monitor *monitor)
{
	const struct monitor_publish *publish = &monitor->publish;
	uint64_t before_us;
	uint64_t after_us;

	assert(read_clock_us(CLOCK_MONOTONIC, &before_us));
	publish_stop(monitor);
	assert(read_clock_us(CLOCK_MONOTONIC, &after_us));
	assert(publish->shared == NULL);
	assert(after_us - before_us < PUBLISH_WAIT_US);
	for (size_t index = 0U; index < message_count; index++)
		assert(strstr(messages[index], "leaving it behind") == NULL);
}

static void test_reply_from_snapshot(void)
{
	struct publish_shared shared = { .pid = 1234U, .started_sec = 1700000000U };
	struct status_snapshot snapshot = {
		.state = CONTROLLER_RUNNING,
		.download = { .interface = "ignored",
			      .available = true,
			      .shaper_bps = 85000000U,
			      .achieved_bps = 61200000U,
			      .load_e6 = 720000U,
			      .delay_us = -150,
			      .bufferbloat = true,
			      .tcp_queue_valid = true,
			      .tcp_queue_us = 2100 },
		.upload = { .interface = "ignored" },
	};
	const char *expected[] = {
		"version=" PROGRAM_VERSION,
		"pid=1234",
		"started=1700000000",
		"state=running",
		"download{",
		"interface=ifb4wan",
		"available=1",
		"shaper_bps=85000000",
		"achieved_bps=61200000",
		"load_e6=720000",
		"delay_us=-150",
		"bufferbloat=1",
		"tcp_queue_us=2100",
		"}",
		"upload{",
		"interface=wan",
		"available=0",
		"}",
	};

	(void)snprintf(shared.download_interface, sizeof(shared.download_interface), "ifb4wan");
	(void)snprintf(shared.upload_interface, sizeof(shared.upload_interface), "wan");
	publish_reply(&shared.reply, &shared, &snapshot);
	assert(field_count == ARRAY_SIZE(expected));
	for (size_t index = 0U; index < field_count; index++)
		assert(strcmp(fields[index], expected[index]) == 0);
}

static void test_stop_without_start(void)
{
	struct monitor monitor = { 0 };

	publish_stop(&monitor);
	publish_update(&monitor);
	assert(monitor.publish.shared == NULL);
}

/*
 * Case 1 and 2, ubusd absent at start: one error at the first failed attempt,
 * none for the retries, control goes on, and the later connection is logged.
 */
static void test_unavailable_then_connected(void)
{
	struct monitor monitor;
	const struct publish_shared *shared;

	reset(&monitor);
	publish_start(&monitor, "main");
	shared = monitor.publish.shared;
	assert(shared != NULL);
	assert(strcmp(shared->object_name, "cake-adapt.main") == 0);
	wait_for_events(&monitor, PUBLISH_NO_CONNECTION);
	publish_update(&monitor);
	assert(message_count == 1U);
	assert_message(0U, LOG_LEVEL_ERROR, "cannot publish status on ubus: no connection");
	wait_for_count(&connects, 2);
	publish_update(&monitor);
	assert(message_count == 1U);

	atomic_store(&ubusd_running, true);
	wait_for_events(&monitor, PUBLISH_CONNECTED);
	publish_update(&monitor);
	assert(message_count == 2U);
	assert_system_message(1U, "connected to ubus; status published as cake-adapt.main");
	assert(!uloop_cancelled);
	stop_and_check(&monitor);
	(void)close(atomic_load(&ubusd_fd));
}

/*
 * Published at the first attempt, silently; a request answered from the latest
 * copy; then case 3 with ubusd gone: the reconnection fails, the next attempt
 * too, and ubusd's return is logged with both IDs cleared before the add.
 */
static void test_publish_serve_lose_recover(void)
{
	struct monitor monitor;

	reset(&monitor);
	atomic_store(&ubusd_running, true);
	publish_start(&monitor, "second");
	wait_for_count(&adds, 1);
	publish_update(&monitor);
	assert(message_count == 0U);

	supplied_snapshot.state = CONTROLLER_STALL;
	publish_update(&monitor);
	assert(write(atomic_load(&ubusd_fd), "r", 1U) == 1);
	wait_for_count(&replies, 1);
	assert(strcmp(fields[3], "state=stall") == 0);
	assert(strcmp(fields[5], "interface=ifb4wan") == 0);

	atomic_store(&ubusd_running, false);
	(void)close(atomic_load(&ubusd_fd));
	wait_for_events(&monitor, PUBLISH_RECONNECT_FAILED | PUBLISH_NO_CONNECTION);
	publish_update(&monitor);
	assert(message_count == 2U);
	assert_message(0U, LOG_LEVEL_ERROR, "cannot reconnect to ubus: Connection failed");
	assert_message(1U, LOG_LEVEL_ERROR, "cannot publish status on ubus: no connection");

	atomic_store(&ubusd_running, true);
	wait_for_events(&monitor, PUBLISH_CONNECTED);
	assert(atomic_load(&object_id_before_add) == 0U);
	assert(atomic_load(&type_id_before_add) == 0U);
	publish_update(&monitor);
	assert(message_count == 3U);
	assert_system_message(2U, "connected to ubus; status published as cake-adapt.second");
	stop_and_check(&monitor);
	(void)close(atomic_load(&ubusd_fd));
}

/* Case 3 with ubusd back at once: the reconnection adds the object again, unlogged. */
static void test_reconnect_at_once(void)
{
	struct monitor monitor;

	reset(&monitor);
	atomic_store(&ubusd_running, true);
	publish_start(&monitor, "main");
	wait_for_count(&adds, 1);
	(void)close(atomic_load(&ubusd_fd));
	wait_for_count(&adds, 2);
	assert(atomic_load(&reconnects) == 1);
	assert(atomic_load(&connects) == 1);
	assert(write(atomic_load(&ubusd_fd), "r", 1U) == 1);
	wait_for_count(&replies, 1);
	publish_update(&monitor);
	assert(message_count == 0U);
	stop_and_check(&monitor);
	(void)close(atomic_load(&ubusd_fd));
}

/* Case 4: a hang-up libubus did not see as the end closes the connection; it is made again. */
static void test_hangup(void)
{
	struct monitor monitor;

	reset(&monitor);
	atomic_store(&ubusd_running, true);
	atomic_store(&end_unnoticed, true);
	publish_start(&monitor, "main");
	wait_for_count(&adds, 1);
	(void)close(atomic_load(&ubusd_fd));
	wait_for_events(&monitor, PUBLISH_DISCONNECTED | PUBLISH_CONNECTED);
	assert(atomic_load(&reconnects) == 0);
	publish_update(&monitor);
	assert(message_count == 2U);
	assert_system_message(0U, "disconnecting from ubus");
	assert_system_message(1U, "connected to ubus; status published as cake-adapt.main");
	stop_and_check(&monitor);
	(void)close(atomic_load(&ubusd_fd));
}

/* Case 5a: ubusd refuses the object on the first connection, so the run ends. */
static void test_add_refused_at_start(void)
{
	struct monitor monitor;

	reset(&monitor);
	monitor.result = 0;
	atomic_store(&ubusd_running, true);
	atomic_store(&add_result, UBUS_STATUS_PERMISSION_DENIED);
	publish_start(&monitor, "main");
	wait_for_events(&monitor, PUBLISH_ADD_FAILED_AT_START);
	publish_update(&monitor);
	assert(message_count == 1U);
	assert_message(0U, LOG_LEVEL_ERROR, "ubus error: Permission denied; exiting");
	assert(monitor.result == -1);
	assert(uloop_cancelled);
	stop_and_check(&monitor);
}

/* Case 5b: refused on a later connection, logged once; no further attempt, control goes on. */
static void test_add_refused_later(void)
{
	struct monitor monitor;

	reset(&monitor);
	atomic_store(&add_result, UBUS_STATUS_PERMISSION_DENIED);
	publish_start(&monitor, "main");
	wait_for_events(&monitor, PUBLISH_NO_CONNECTION);
	atomic_store(&ubusd_running, true);
	wait_for_events(&monitor, PUBLISH_ADD_FAILED);
	assert_no_attempt();
	publish_update(&monitor);
	assert(message_count == 2U);
	assert_message(0U, LOG_LEVEL_ERROR, "cannot publish status on ubus: no connection");
	assert_message(1U, LOG_LEVEL_ERROR, "ubus error: Permission denied; status not published");
	assert(monitor.result == 0);
	assert(!uloop_cancelled);
	stop_and_check(&monitor);
}

/* Case 5b after a reconnection: the add libubus repeats is refused, so the thread stops trying. */
static void test_add_refused_on_reconnect(void)
{
	struct monitor monitor;

	reset(&monitor);
	atomic_store(&ubusd_running, true);
	publish_start(&monitor, "main");
	wait_for_count(&adds, 1);
	atomic_store(&add_result, UBUS_STATUS_PERMISSION_DENIED);
	(void)close(atomic_load(&ubusd_fd));
	wait_for_events(&monitor, PUBLISH_ADD_FAILED);
	assert_no_attempt();
	publish_update(&monitor);
	assert(message_count == 1U);
	assert_message(0U, LOG_LEVEL_ERROR, "ubus error: Permission denied; status not published");
	assert(!uloop_cancelled);
	stop_and_check(&monitor);
}

/* A thread stuck in libubus is left behind after about PUBLISH_STOP_US. */
static void test_stuck_thread_left_behind(void)
{
	struct monitor monitor;
	struct publish_shared *shared;
	uint64_t before_us;
	uint64_t after_us;

	reset(&monitor);
	atomic_store(&connect_hangs, true);
	atomic_store(&thread_finished, false);
	publish_start(&monitor, "main");
	shared = monitor.publish.shared;
	while (atomic_load(&connects) == 0)
		pause_step();
	assert(read_clock_us(CLOCK_MONOTONIC, &before_us));
	publish_stop(&monitor);
	assert(read_clock_us(CLOCK_MONOTONIC, &after_us));
	assert(monitor.publish.shared == NULL);
	assert(after_us - before_us >= PUBLISH_STOP_US);
	assert(after_us - before_us < PUBLISH_STOP_US + 500U * MILLISECOND);
	assert(message_count == 1U);
	assert(strcmp(messages[0],
		      "ubus status thread did not stop within 2.0 s; leaving it behind") == 0);

	/* The detached thread sees the stop once libubus returns; then the test frees its state. */
	atomic_store(&connect_hangs, false);
	while (!atomic_load(&thread_finished))
		pause_step();
	pause_step();
	publish_free(shared);
}

int main(void)
{
	test_reply_from_snapshot();
	test_stop_without_start();
	test_unavailable_then_connected();
	test_publish_serve_lose_recover();
	test_reconnect_at_once();
	test_hangup();
	test_add_refused_at_start();
	test_add_refused_later();
	test_add_refused_on_reconnect();
	test_stuck_thread_left_behind();
	puts("publish tests passed");
	return 0;
}
