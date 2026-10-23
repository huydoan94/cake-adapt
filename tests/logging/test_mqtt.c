/*
 * Inspect the queue seam while linking the actual Mosquitto API. Connection, loop, publish and
 * destroy are replaced so the sender's threads can run without a broker or a resolver.
 */
#define _GNU_SOURCE
#include "config/config.h"
#include <mosquitto.h>
#include <stdio.h>

#if MQTT_TLS_SUPPORTED
static unsigned int tls_calls;
static const char *tls_ca_file;

static int test_tls_setup(
	struct mosquitto *client,
	const char *ca_file,
	const char *ca_path,
	const char *certificate,
	const char *key,
	int (*password)(char *, int, int, void *)
)
{
	(void)client;
	(void)ca_path;
	(void)certificate;
	(void)key;
	(void)password;
	tls_calls++;
	tls_ca_file = ca_file;
	return MOSQ_ERR_INVAL;
}
#define mosquitto_tls_set test_tls_setup
#endif

static int mock_connect(struct mosquitto *client, const char *host, int port, int keepalive);
static int mock_loop(struct mosquitto *client, int timeout_ms, int max_packets);
static int mock_publish(
	struct mosquitto *client,
	int *identifier,
	const char *topic,
	int length,
	const void *payload,
	int qos,
	bool retain
);
static void mock_destroy(struct mosquitto *client);
static int mock_lib_cleanup(void);
#define mosquitto_connect_async mock_connect
#define mosquitto_loop mock_loop
#define mosquitto_publish mock_publish
#define mosquitto_destroy mock_destroy
#define mosquitto_lib_cleanup mock_lib_cleanup
#include "logging/mqtt.c"
#include "constants/mqtt.h"
#include "constants/options.h"
#include "constants/program.h"
#include "constants/text.h"
#include "constants/units.h"
#undef mosquitto_connect_async
#undef mosquitto_loop
#undef mosquitto_publish
#undef mosquitto_destroy
#undef mosquitto_lib_cleanup
#if MQTT_TLS_SUPPORTED
#undef mosquitto_tls_set
#endif

#include <assert.h>

#define MOCK_RECORDS 16U
#define MOCK_STEP_US MILLISECOND
#define MOCK_LOOP_US MILLISECOND
/* The broker's write completes on the network thread this long after the publish call. */
#define MOCK_PUBLISH_DELAY_US (2U * MILLISECOND)
#define MOCK_BATCH 8U
/* Well within MQTT_WAIT_MS: a lost wake-up costs a whole poll timeout per line. */
#define MOCK_BATCH_BOUND_US (50U * MILLISECOND)

static pthread_mutex_t mock_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mock_resolved = PTHREAD_COND_INITIALIZER;
/* The next connection's "name resolution" blocks until mock_release is set. */
static bool mock_block_next;
static bool mock_release;
static unsigned int mock_resolving;
static struct mosquitto *mock_announced;
static unsigned int mock_published;
static unsigned int mock_connects;
static struct mosquitto *mock_publishing;
static uint64_t mock_publish_due_us;
static char mock_topic[CONFIG_STRING_SIZE];
static char mock_records[MOCK_RECORDS][MQTT_LINE_SIZE];
static atomic_uint mock_destroyed;
static atomic_uint mock_cleanups;

static int mock_connect(struct mosquitto *client, const char *host, int port, int keepalive)
{
	(void)client;
	(void)host;
	(void)port;
	(void)keepalive;
	(void)pthread_mutex_lock(&mock_lock);
	mock_connects++;
	if (mock_block_next) {
		mock_block_next = false;
		mock_resolving++;
		while (!mock_release)
			(void)pthread_cond_wait(&mock_resolved, &mock_lock);
		mock_resolving--;
	}
	(void)pthread_mutex_unlock(&mock_lock);
	return MOSQ_ERR_SUCCESS;
}

/*
 * The broker accepts a new client on its first loop pass, and a publish completes on a later
 * pass, as libmosquitto reports both from the network thread.
 */
static int mock_loop(struct mosquitto *client, int timeout_ms, int max_packets)
{
	const struct timespec step = us_to_timespec(MOCK_LOOP_US);
	uint64_t now_us = 0U;
	bool announce;
	bool published;

	(void)timeout_ms;
	(void)max_packets;
	(void)read_clock_us(CLOCK_MONOTONIC, &now_us);
	(void)pthread_mutex_lock(&mock_lock);
	announce = mock_announced != client;
	mock_announced = client;
	published = mock_publishing == client && now_us >= mock_publish_due_us;
	if (published)
		mock_publishing = NULL;
	(void)pthread_mutex_unlock(&mock_lock);
	if (announce)
		mqtt_connected(client, mosquitto_userdata(client), MOSQ_ERR_SUCCESS);
	if (published)
		mqtt_published(client, mosquitto_userdata(client), 0);
	(void)clock_nanosleep(CLOCK_MONOTONIC, 0, &step, NULL);
	return MOSQ_ERR_SUCCESS;
}

static int mock_publish(
	struct mosquitto *client,
	int *identifier,
	const char *topic,
	int length,
	const void *payload,
	int qos,
	bool retain
)
{
	(void)identifier;
	(void)qos;
	(void)retain;
	if (client == NULL)
		return MOSQ_ERR_INVAL;
	(void)pthread_mutex_lock(&mock_lock);
	assert(mock_published < MOCK_RECORDS);
	(void)snprintf(mock_topic, sizeof(mock_topic), "%s", topic);
	(void)snprintf(
		mock_records[mock_published],
		sizeof(mock_records[0]),
		"%.*s",
		length,
		(const char *)payload
	);
	mock_published++;
	/* The sender allows one packet in flight, so one completion is ever outstanding. */
	assert(mock_publishing == NULL);
	mock_publishing = client;
	assert(read_clock_us(CLOCK_MONOTONIC, &mock_publish_due_us));
	mock_publish_due_us += MOCK_PUBLISH_DELAY_US;
	(void)pthread_mutex_unlock(&mock_lock);
	return MOSQ_ERR_SUCCESS;
}

static void mock_destroy(struct mosquitto *client)
{
	mosquitto_destroy(client);
	atomic_fetch_add(&mock_destroyed, 1U);
}

static int mock_lib_cleanup(void)
{
	atomic_fetch_add(&mock_cleanups, 1U);
	return mosquitto_lib_cleanup();
}

static void mock_reset(void)
{
	(void)pthread_mutex_lock(&mock_lock);
	mock_block_next = true;
	mock_release = false;
	mock_announced = NULL;
	mock_published = 0U;
	mock_connects = 0U;
	mock_publishing = NULL;
	(void)pthread_mutex_unlock(&mock_lock);
	atomic_store(&mock_destroyed, 0U);
	atomic_store(&mock_cleanups, 0U);
}

static void mock_resolve(void)
{
	(void)pthread_mutex_lock(&mock_lock);
	mock_release = true;
	(void)pthread_cond_broadcast(&mock_resolved);
	(void)pthread_mutex_unlock(&mock_lock);
}

static unsigned int mock_count(unsigned int *counter)
{
	unsigned int value;

	(void)pthread_mutex_lock(&mock_lock);
	value = *counter;
	(void)pthread_mutex_unlock(&mock_lock);
	return value;
}

static bool mock_online(void)
{
	struct mqtt_sender *state = sender;

	return state != NULL && atomic_load(&state->online);
}

static uint64_t now(void)
{
	uint64_t now_us = 0U;

	assert(read_clock_us(CLOCK_MONOTONIC, &now_us));
	return now_us;
}

static void pause_step(void)
{
	const struct timespec step = us_to_timespec(MOCK_STEP_US);

	(void)clock_nanosleep(CLOCK_MONOTONIC, 0, &step, NULL);
}

static void wait_counter(unsigned int *counter, unsigned int expected, uint64_t bound_us)
{
	uint64_t start_us = now();

	while (mock_count(counter) != expected) {
		assert(now() - start_us < bound_us);
		pause_step();
	}
}

static void wait_atomic(atomic_uint *counter, unsigned int expected, uint64_t bound_us)
{
	uint64_t start_us = now();

	while (atomic_load(counter) != expected) {
		assert(now() - start_us < bound_us);
		pause_step();
	}
}

static void test_queue(void)
{
	struct mqtt_sender state = { 0 };
	struct mqtt_connection connection = { .state = &state };
	char boundary[MQTT_LINE_SIZE + 1U];
	unsigned int index;

	assert(pthread_mutex_init(&state.lock, NULL) == 0);
	state.wake_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
	assert(state.wake_fd >= 0);
	atomic_init(&state.online, false);
	atomic_init(&connection.online, true);
	atomic_init(&connection.pending, false);
	sender = &state;
	mqtt_submit("offline");
	assert(state.count == 0U);
	atomic_store(&state.online, true);
	mqtt_submit("DATA; exact payload");
	assert(state.count == 1U);
	assert(strcmp(state.lines[0], "DATA; exact payload") == 0);

	/* Neither a held mutex nor a full queue may block the caller. */
	assert(pthread_mutex_lock(&state.lock) == 0);
	mqtt_submit("contended");
	assert(state.count == 1U);
	assert(pthread_mutex_unlock(&state.lock) == 0);
	for (index = 1U; index < 32U; index++)
		mqtt_submit("queued");
	assert(state.count == 32U);
	mqtt_submit("full");
	assert(state.count == 32U);
	assert(strcmp(state.lines[0], "DATA; exact payload") == 0);
	mqtt_discard(&state);
	assert(state.count == 0U && !atomic_load(&state.online));
	atomic_store(&state.online, true);
	memset(boundary, 'x', sizeof(boundary));
	boundary[2048] = '\0';
	mqtt_submit(boundary);
	assert(state.count == 0U);
	boundary[2047] = '\0';
	mqtt_submit(boundary);
	assert(state.count == 1U && strlen(state.lines[0]) == 2047U);

	/* A library send failure disables acceptance and loses the queued record. */
	mqtt_send(&state, &connection);
	assert(state.count == 0U && !atomic_load(&state.online));
	assert(!atomic_load(&connection.online) && !atomic_load(&connection.pending));
	sender = NULL;
	(void)close(state.wake_fd);
	(void)pthread_mutex_destroy(&state.lock);
}

/* Only the line that makes the queue non-empty wakes the sender; dropped lines never do. */
static void test_wake(void)
{
	struct mqtt_sender state = { 0 };
	eventfd_t wakes;
	unsigned int index;

	assert(pthread_mutex_init(&state.lock, NULL) == 0);
	state.wake_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
	assert(state.wake_fd >= 0);
	atomic_init(&state.online, false);
	sender = &state;
	mqtt_submit("offline");
	assert(eventfd_read(state.wake_fd, &wakes) == -1 && errno == EAGAIN);
	atomic_store(&state.online, true);
	for (index = 0U; index < 5U; index++)
		mqtt_submit("batched");
	assert(state.count == 5U);
	assert(eventfd_read(state.wake_fd, &wakes) == 0 && wakes == 1U);
	for (index = 5U; index < 33U; index++)
		mqtt_submit("filling");
	assert(state.count == 32U);
	assert(eventfd_read(state.wake_fd, &wakes) == -1 && errno == EAGAIN);
	mqtt_discard(&state);
	atomic_store(&state.online, true);
	mqtt_submit("first after drain");
	mqtt_submit("second after drain");
	assert(eventfd_read(state.wake_fd, &wakes) == 0 && wakes == 1U);
	sender = NULL;
	(void)close(state.wake_fd);
	(void)pthread_mutex_destroy(&state.lock);
}

/*
 * A connection stuck resolving is abandoned after MQTT_TIMEOUT_US without cancellation, and no
 * new attempt starts while it still resolves, so at most one thread is ever left behind. Once
 * it returns, it destroys only its own client and the next attempt delivers a whole batch,
 * each line woken by the publish completion rather than by a poll timeout.
 */
static void test_stuck_resolver(void)
{
	const struct mqtt_settings settings = {
		.enabled = true,
		.host = "localhost",
		.topic = "cake-adapt/test",
		.port = 1883U,
	};
	char error[ERROR_SIZE] = { 0 };
	char record[MQTT_LINE_SIZE];
	uint64_t start_us;
	unsigned int index;

	mock_reset();
	start_us = now();
	assert(mqtt_start(&settings, error, sizeof(error)) == 0);
	wait_counter(&mock_resolving, 1U, SECOND);
	while (now() - start_us < MQTT_RETRY_US + 500U * MILLISECOND)
		pause_step();
	/* Abandoned at the timeout, and still the only attempt past the retry time. */
	assert(mock_count(&mock_connects) == 1U && mock_count(&mock_resolving) == 1U);
	assert(!mock_online() && atomic_load(&mock_destroyed) == 0U);

	mock_resolve();
	wait_atomic(&mock_destroyed, 1U, SECOND);
	wait_counter(&mock_resolving, 0U, SECOND);
	start_us = now();
	while (!mock_online()) {
		assert(now() - start_us < SECOND);
		pause_step();
	}
	assert(mock_count(&mock_connects) == 2U);

	for (index = 0U; index < MOCK_BATCH; index++) {
		(void)snprintf(record, sizeof(record), "BATCH; %u", index);
		mqtt_submit(record);
	}
	wait_counter(&mock_published, MOCK_BATCH, MOCK_BATCH_BOUND_US);
	for (index = 0U; index < MOCK_BATCH; index++) {
		(void)snprintf(record, sizeof(record), "BATCH; %u", index);
		assert(strcmp(mock_records[index], record) == 0);
	}
	assert(strcmp(mock_topic, "cake-adapt/test") == 0);
	mqtt_stop();
	wait_atomic(&mock_destroyed, 2U, SECOND);
	wait_atomic(&mock_cleanups, 1U, SECOND);
}

/* Shutdown stays bounded while resolving; the left-behind thread frees everything later. */
static void test_stop_while_resolving(void)
{
	const struct timespec settle = us_to_timespec(100U * MILLISECOND);
	const struct mqtt_settings settings = {
		.enabled = true,
		.host = "localhost",
		.topic = "cake-adapt/test",
		.port = 1883U,
	};
	char error[ERROR_SIZE] = { 0 };
	uint64_t start_us;

	mock_reset();
	assert(mqtt_start(&settings, error, sizeof(error)) == 0);
	wait_counter(&mock_resolving, 1U, SECOND);
	start_us = now();
	mqtt_stop();
	assert(now() - start_us <= MQTT_STOP_US + 100U * MILLISECOND);
	assert(!mqtt_enabled());
	assert(atomic_load(&mock_destroyed) == 0U && atomic_load(&mock_cleanups) == 0U);
	mock_resolve();
	wait_atomic(&mock_destroyed, 1U, SECOND);
	/* The last owner cleans up the library, then frees the state. */
	wait_atomic(&mock_cleanups, 1U, SECOND);
	(void)clock_nanosleep(CLOCK_MONOTONIC, 0, &settle, NULL);
}

static void test_tls(void)
{
	struct mqtt_sender state = { .settings = { .enabled = true,
						   .tls = true,
						   .host = "localhost",
						   .ca_file = "/tmp/fixture-ca.pem" } };

#if MQTT_TLS_SUPPORTED
	assert(mosquitto_lib_init() == MOSQ_ERR_SUCCESS);
	assert(mqtt_connect(&state) == NULL);
	assert(tls_calls == 1U);
	assert(strcmp(tls_ca_file, "/tmp/fixture-ca.pem") == 0);
	/* A TLS setup failure never starts the network thread or retries as plain MQTT. */
	assert(atomic_load(&state.owners) == 0U);
	assert(!atomic_load(&state.online));
	assert(mosquitto_lib_cleanup() == MOSQ_ERR_SUCCESS);
#else
	char error[ERROR_SIZE] = { 0 };

	assert(mqtt_start(&state.settings, error, sizeof(error)) == -1);
	assert(strstr(error, "TLS") != NULL);
	assert(!mqtt_enabled());
#endif
}

int main(void)
{
	const struct mqtt_settings disabled = { 0 };
	char error[ERROR_SIZE] = { 0 };

	assert(mqtt_start(&disabled, error, sizeof(error)) == 0);
	assert(!mqtt_enabled());
	mqtt_submit("disabled");
	mqtt_stop();
	test_queue();
	test_wake();
	test_tls();
	test_stop_while_resolving();
	test_stuck_resolver();
	(void)puts("MQTT queue, wake, stuck-resolver, disabled and TLS setup tests passed");
	return 0;
}
