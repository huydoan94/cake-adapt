#define _GNU_SOURCE

#include "logging/mqtt.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "constants/mqtt.h"
#include "constants/program.h"

#include <mosquitto.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

struct mqtt_sender;

/*
 * One connection attempt: its client, network thread and flags. The sender and the network
 * thread each hold a reference and the last one destroys the client, so a network thread left
 * behind in the resolver never touches a destroyed client, and its late callbacks only reach
 * its own flags, never a newer connection's.
 */
struct mqtt_connection {
	struct mqtt_sender *state;
	struct mosquitto *client;
	pthread_t thread;
	atomic_uint owners;
	atomic_bool stop;
	atomic_bool online;
	atomic_bool pending;
};

/* The main thread, the sender thread and every network thread each hold a reference. */
struct mqtt_sender {
	struct mqtt_settings settings;
	pthread_t thread;
	pthread_mutex_t lock;
	atomic_uint owners;
	atomic_bool stopping;
	/* The sender's view of its current connection; mqtt_submit accepts only while set. */
	atomic_bool online;
	int wake_fd;
	unsigned int first;
	unsigned int count;
	char lines[MQTT_QUEUE_RECORDS][MQTT_LINE_SIZE];
};

static struct mqtt_sender *sender;

static void mqtt_release(struct mqtt_sender *state)
{
	if (atomic_fetch_sub(&state->owners, 1U) != 1U)
		return;
	(void)mosquitto_lib_cleanup();
	(void)close(state->wake_fd);
	(void)pthread_mutex_destroy(&state->lock);
	free(state);
}

static void mqtt_connection_release(struct mqtt_connection *connection)
{
	if (atomic_fetch_sub(&connection->owners, 1U) != 1U)
		return;
	if (connection->client != NULL)
		mosquitto_destroy(connection->client);
	free(connection);
}

/* Mosquitto's opaque client exposes its owner through its native callback API. */
static void mqtt_connected(struct mosquitto *client, void *owner, int result)
{
	struct mqtt_connection *connection = owner;
	struct mqtt_sender *state = connection->state;

	(void)client;
	atomic_store(&connection->online, result == 0);
	(void)eventfd_write(state->wake_fd, 1U);
}

static void mqtt_disconnected(struct mosquitto *client, void *owner, int result)
{
	struct mqtt_connection *connection = owner;
	struct mqtt_sender *state = connection->state;

	(void)client;
	(void)result;
	atomic_store(&connection->online, false);
	(void)eventfd_write(state->wake_fd, 1U);
}

static void mqtt_published(struct mosquitto *client, void *owner, int identifier)
{
	struct mqtt_connection *connection = owner;
	struct mqtt_sender *state = connection->state;

	(void)client;
	(void)identifier;
	atomic_store(&connection->pending, false);
	(void)eventfd_write(state->wake_fd, 1U);
}

static void mqtt_discard(struct mqtt_sender *state)
{
	atomic_store(&state->online, false);
	(void)pthread_mutex_lock(&state->lock);
	state->first = 0U;
	state->count = 0U;
	(void)pthread_mutex_unlock(&state->lock);
}

static void *mqtt_network(void *argument)
{
	struct mqtt_connection *connection = argument;
	struct mqtt_sender *state = connection->state;
	const struct mqtt_settings *settings = &state->settings;
	/* Name resolution happens here and can block; the sender never waits for it unbounded. */
	int result = mosquitto_connect_async(
		connection->client,
		settings->host,
		(int)settings->port,
		MQTT_KEEPALIVE_SEC
	);

	if (result == MOSQ_ERR_SUCCESS || result == MOSQ_ERR_CONN_PENDING)
		while (!atomic_load(&connection->stop) &&
		       mosquitto_loop(connection->client, us_to_timeout_ms(MQTT_WAIT_US), 1) ==
			       MOSQ_ERR_SUCCESS)
			;
	atomic_store(&connection->online, false);
	(void)eventfd_write(state->wake_fd, 1U);
	mqtt_connection_release(connection);
	mqtt_release(state);
	return NULL;
}

static struct mqtt_connection *mqtt_connect(struct mqtt_sender *state)
{
	const struct mqtt_settings *settings = &state->settings;
	bool clean_session = true;
	bool threaded = true;
	struct mqtt_connection *connection = calloc(1U, sizeof(*connection));
	struct mosquitto *client;

	if (connection == NULL)
		return NULL;
	connection->state = state;
	atomic_init(&connection->owners, 1U);
	atomic_init(&connection->stop, false);
	atomic_init(&connection->online, false);
	atomic_init(&connection->pending, false);
	client = mosquitto_new(NULL, clean_session, connection);
	if (client == NULL)
		goto failed;
	connection->client = client;
	mosquitto_connect_callback_set(client, mqtt_connected);
	mosquitto_disconnect_callback_set(client, mqtt_disconnected);
	mosquitto_publish_callback_set(client, mqtt_published);
	if (mosquitto_threaded_set(client, threaded) != MOSQ_ERR_SUCCESS)
		goto failed;
	if (settings->username[0] != '\0' &&
	    mosquitto_username_pw_set(client, settings->username, settings->password) !=
		    MOSQ_ERR_SUCCESS)
		goto failed;
#if MQTT_TLS_SUPPORTED
	/* Mosquitto defaults to peer certificate and hostname verification. */
	if (settings->tls && mosquitto_tls_set(client, settings->ca_file, NULL, NULL, NULL, NULL) !=
				     MOSQ_ERR_SUCCESS)
		goto failed;
#endif
	/* The network thread's own references; the sender's keeps the state alive meanwhile. */
	atomic_fetch_add(&connection->owners, 1U);
	atomic_fetch_add(&state->owners, 1U);
	if (pthread_create(&connection->thread, NULL, mqtt_network, connection) != 0) {
		atomic_fetch_sub(&state->owners, 1U);
		atomic_fetch_sub(&connection->owners, 1U);
		goto failed;
	}
	return connection;
failed:
	mqtt_connection_release(connection);
	return NULL;
}

static void mqtt_destroy(struct mqtt_sender *state, struct mqtt_connection *connection)
{
	mqtt_discard(state);
	atomic_store(&connection->stop, true);
	/*
	 * Never cancel: cancelling inside the resolver can leak. A thread still resolving after
	 * the bound is left behind with its own references and destroys its client on exit.
	 */
	if (!thread_join_within(connection->thread, MQTT_NETWORK_STOP_US))
		(void)pthread_detach(connection->thread);
	mqtt_connection_release(connection);
}

static void mqtt_send(struct mqtt_sender *state, struct mqtt_connection *connection)
{
	const struct mqtt_settings *settings = &state->settings;
	char line[MQTT_LINE_SIZE];
	bool retain = false;

	/* At most one QoS 0 packet can be waiting inside libmosquitto. */
	if (atomic_load(&connection->pending))
		return;
	(void)pthread_mutex_lock(&state->lock);
	if (state->count == 0U) {
		(void)pthread_mutex_unlock(&state->lock);
		return;
	}
	(void)strcpy(line, state->lines[state->first]);
	state->first = (state->first + 1U) % MQTT_QUEUE_RECORDS;
	state->count--;
	(void)pthread_mutex_unlock(&state->lock);
	atomic_store(&connection->pending, true);
	if (mosquitto_publish(
		    connection->client,
		    NULL,
		    settings->topic,
		    (int)strlen(line),
		    line,
		    0,
		    retain
	    ) != MOSQ_ERR_SUCCESS) {
		atomic_store(&connection->pending, false);
		atomic_store(&connection->online, false);
		atomic_store(&state->online, false);
	}
}

static void *mqtt_thread(void *argument)
{
	struct mqtt_sender *state = argument;
	struct mqtt_connection *connection = NULL;
	uint64_t retry_us = 0U;
	uint64_t progress_us = 0U;
	uint64_t now_us;
	bool was_online = false;
	struct pollfd wake = { .fd = state->wake_fd, .events = POLLIN };
	eventfd_t value;

	while (!atomic_load(&state->stopping)) {
		if (!read_clock_us(CLOCK_MONOTONIC, &now_us))
			goto idle;
		/* A thread left behind in the resolver blocks new attempts until it returns. */
		if (connection == NULL && now_us >= retry_us &&
		    atomic_load(&state->owners) <= MQTT_SENDER_OWNERS) {
			retry_us = saturating_add(now_us, MQTT_RETRY_US);
			progress_us = now_us;
			connection = mqtt_connect(state);
		}
		if (connection != NULL) {
			bool online = atomic_load(&connection->online);

			if (online && !atomic_load(&connection->pending))
				progress_us = now_us;
			if ((!online && was_online) || now_us - progress_us >= MQTT_TIMEOUT_US) {
				mqtt_destroy(state, connection);
				connection = NULL;
				if (was_online)
					retry_us = saturating_add(now_us, MQTT_RETRY_US);
				online = false;
			} else {
				atomic_store(&state->online, online);
				if (online)
					mqtt_send(state, connection);
			}
			was_online = online;
		}
		/*
		 * Drain a bounded batch promptly without allowing the library queue to grow.
		 * mqtt_submit wakes only an empty queue; a line queued meanwhile is sent here or
		 * after the publish callback's wake.
		 */
		if (connection != NULL && atomic_load(&connection->online) &&
		    !atomic_load(&connection->pending)) {
			(void)pthread_mutex_lock(&state->lock);
			bool queued = state->count != 0U;
			(void)pthread_mutex_unlock(&state->lock);
			if (queued)
				continue;
		}
idle:
		if (poll(&wake, 1U, us_to_timeout_ms(MQTT_WAIT_US)) > 0)
			(void)eventfd_read(state->wake_fd, &value);
	}
	if (connection != NULL)
		mqtt_destroy(state, connection);
	mqtt_release(state);
	return NULL;
}

int mqtt_start(const struct mqtt_settings *settings, char *error, size_t error_size)
{
	struct mqtt_sender *state;
	sigset_t all;
	sigset_t previous;
	int result;

	if (!settings->enabled)
		return 0;
#if !MQTT_TLS_SUPPORTED
	if (settings->tls)
		return error_set(error, error_size, "TLS is not supported by this build");
#endif
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return error_set(error, error_size, "allocating the sender: %s", strerror(errno));
	state->settings = *settings;
	atomic_init(&state->owners, MQTT_SENDER_OWNERS);
	atomic_init(&state->stopping, false);
	atomic_init(&state->online, false);
	if ((result = pthread_mutex_init(&state->lock, NULL)) != 0) {
		(void)error_set(error, error_size, "creating the queue lock: %s", strerror(result));
		goto free_state;
	}
	state->wake_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
	if (state->wake_fd < 0) {
		(void)error_set(error, error_size, "creating the wake eventfd: %s", strerror(errno));
		goto destroy_lock;
	}
	/* The library's last user is whichever owner releases the state last. */
	if ((result = mosquitto_lib_init()) != MOSQ_ERR_SUCCESS) {
		(void)error_set(
			error,
			error_size,
			"initializing libmosquitto: %s",
			mosquitto_strerror(result)
		);
		goto close_wake;
	}
	/* Signals belong to the monitor, including while the library creates its network thread. */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	result = pthread_create(&state->thread, NULL, mqtt_thread, state);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	if (result != 0) {
		atomic_store(&state->owners, 1U);
		mqtt_release(state);
		return error_set(
			error,
			error_size,
			"creating the sender thread: %s",
			strerror(result)
		);
	}
	sender = state;
	return 0;

close_wake:
	(void)close(state->wake_fd);
destroy_lock:
	(void)pthread_mutex_destroy(&state->lock);
free_state:
	free(state);
	return -1;
}

bool mqtt_enabled(void)
{
	return sender != NULL;
}

void mqtt_submit(const char *line)
{
	struct mqtt_sender *state = sender;
	size_t length;
	unsigned int slot;
	bool wake = false;

	if (state == NULL || !atomic_load(&state->online))
		return;
	length = strnlen(line, MQTT_LINE_SIZE);
	if (length == MQTT_LINE_SIZE || pthread_mutex_trylock(&state->lock) != 0)
		return;
	if (atomic_load(&state->online) && state->count < MQTT_QUEUE_RECORDS) {
		slot = (state->first + state->count) % MQTT_QUEUE_RECORDS;
		memcpy(state->lines[slot], line, length + 1U);
		/* The sender drains a non-empty queue without waiting, so only the first line wakes. */
		wake = state->count == 0U;
		state->count++;
	}
	(void)pthread_mutex_unlock(&state->lock);
	if (wake)
		(void)eventfd_write(state->wake_fd, 1U);
}

void mqtt_stop(void)
{
	struct mqtt_sender *state = sender;

	if (state == NULL)
		return;
	sender = NULL;
	atomic_store(&state->stopping, true);
	(void)eventfd_write(state->wake_fd, 1U);
	/* The thread keeps its own reference until it exits; shutdown remains bounded. */
	if (!thread_join_within(state->thread, MQTT_STOP_US))
		(void)pthread_detach(state->thread);
	mqtt_release(state);
}
