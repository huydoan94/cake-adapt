#define _GNU_SOURCE

#include "logging/mqtt.h"
#include "config/config.h"
#include "common/helpers.h"
#include "common/utils.h"

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

#define MQTT_QUEUE_RECORDS 32U
#define MQTT_LINE_SIZE 2048U
#define MQTT_RETRY_US (10U * SECOND)
#define MQTT_TIMEOUT_US (5U * SECOND)
#define MQTT_WAIT_MS 100
#define MQTT_STOP_CHECK_US (10U * MILLISECOND)
#define MQTT_STOP_US SECOND
#define MQTT_KEEPALIVE_SEC 30

struct mqtt_sender {
	struct config_mqtt settings;
	pthread_t thread;
	pthread_t network_thread;
	struct mosquitto *client;
	pthread_mutex_t lock;
	atomic_uint owners;
	atomic_bool stopping;
	atomic_bool online;
	atomic_bool pending;
	atomic_bool connecting;
	atomic_bool network_stop;
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
	(void)close(state->wake_fd);
	(void)pthread_mutex_destroy(&state->lock);
	free(state);
}

/* Mosquitto's opaque client exposes its owner through its native callback API. */
static void mqtt_connected(struct mosquitto *client, void *owner, int result)
{
	struct mqtt_sender *state = owner;

	(void)client;
	atomic_store(&state->online, result == 0);
	(void)eventfd_write(state->wake_fd, 1U);
}

static void mqtt_disconnected(struct mosquitto *client, void *owner, int result)
{
	struct mqtt_sender *state = owner;

	(void)client;
	(void)result;
	atomic_store(&state->online, false);
	(void)eventfd_write(state->wake_fd, 1U);
}

static void mqtt_published(struct mosquitto *client, void *owner, int identifier)
{
	struct mqtt_sender *state = owner;

	(void)client;
	(void)identifier;
	atomic_store(&state->pending, false);
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
	struct mqtt_sender *state = argument;
	const struct config_mqtt *settings = &state->settings;
	int result = mosquitto_connect_async(
		state->client,
		settings->host,
		(int)settings->port,
		MQTT_KEEPALIVE_SEC
	);

	atomic_store(&state->connecting, false);
	if (result == MOSQ_ERR_SUCCESS || result == MOSQ_ERR_CONN_PENDING)
		while (!atomic_load(&state->stopping) && !atomic_load(&state->network_stop) &&
		       mosquitto_loop(state->client, MQTT_WAIT_MS, 1) == MOSQ_ERR_SUCCESS)
			;
	atomic_store(&state->online, false);
	(void)eventfd_write(state->wake_fd, 1U);
	return NULL;
}

static struct mosquitto *mqtt_connect(struct mqtt_sender *state)
{
	const struct config_mqtt *settings = &state->settings;
	bool clean_session = true;
	bool threaded = true;
	struct mosquitto *client = mosquitto_new(NULL, clean_session, state);

	if (client == NULL)
		return NULL;
	mosquitto_connect_callback_set(client, mqtt_connected);
	mosquitto_disconnect_callback_set(client, mqtt_disconnected);
	mosquitto_publish_callback_set(client, mqtt_published);
	if (mosquitto_threaded_set(client, threaded) != MOSQ_ERR_SUCCESS)
		goto failed;
	if (settings->username[0] != '\0' &&
	    mosquitto_username_pw_set(client, settings->username, settings->password) !=
		    MOSQ_ERR_SUCCESS)
		goto failed;
	state->client = client;
	atomic_store(&state->connecting, true);
	atomic_store(&state->network_stop, false);
	if (pthread_create(&state->network_thread, NULL, mqtt_network, state) != 0)
		goto failed;
	return client;
failed:
	mosquitto_destroy(client);
	return NULL;
}

static void mqtt_destroy(struct mqtt_sender *state, struct mosquitto *client)
{
	mqtt_discard(state);
	atomic_store(&state->network_stop, true);
	/* TCP connection establishment is nonblocking; only DNS can need cancellation. */
	if (atomic_load(&state->connecting))
		(void)pthread_cancel(state->network_thread);
	(void)pthread_join(state->network_thread, NULL);
	mosquitto_destroy(client);
	atomic_store(&state->pending, false);
}

static void mqtt_send(struct mqtt_sender *state, struct mosquitto *client)
{
	char line[MQTT_LINE_SIZE];
	bool retain = false;

	/* At most one QoS 0 packet can be waiting inside libmosquitto. */
	if (atomic_load(&state->pending))
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
	atomic_store(&state->pending, true);
	if (mosquitto_publish(
		    client,
		    NULL,
		    state->settings.topic,
		    (int)strlen(line),
		    line,
		    0,
		    retain
	    ) != MOSQ_ERR_SUCCESS) {
		atomic_store(&state->pending, false);
		atomic_store(&state->online, false);
	}
}

static void *mqtt_thread(void *argument)
{
	struct mqtt_sender *state = argument;
	struct mosquitto *client = NULL;
	uint64_t retry_us = 0U;
	uint64_t progress_us = 0U;
	uint64_t now_us;
	bool was_online = false;
	struct pollfd wake = { .fd = state->wake_fd, .events = POLLIN };
	eventfd_t value;

	if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS)
		goto out;
	while (!atomic_load(&state->stopping)) {
		if (!read_clock_us(CLOCK_MONOTONIC, &now_us))
			goto wait;
		if (client == NULL && now_us >= retry_us) {
			retry_us = saturating_add(now_us, MQTT_RETRY_US);
			progress_us = now_us;
			client = mqtt_connect(state);
		}
		if (client != NULL) {
			bool online = atomic_load(&state->online);

			if (online && !atomic_load(&state->pending))
				progress_us = now_us;
			if ((!online && was_online) || now_us - progress_us >= MQTT_TIMEOUT_US) {
				mqtt_destroy(state, client);
				client = NULL;
				if (was_online)
					retry_us = saturating_add(now_us, MQTT_RETRY_US);
				online = false;
			} else if (online) {
				mqtt_send(state, client);
			}
			was_online = online;
		}
wait:
		/* Drain a bounded batch promptly without allowing the library queue to grow. */
		if (client != NULL && atomic_load(&state->online) &&
		    !atomic_load(&state->pending)) {
			(void)pthread_mutex_lock(&state->lock);
			bool queued = state->count != 0U;
			(void)pthread_mutex_unlock(&state->lock);
			if (queued)
				continue;
		}
		if (poll(&wake, 1U, MQTT_WAIT_MS) > 0)
			(void)eventfd_read(state->wake_fd, &value);
	}
	if (client != NULL)
		mqtt_destroy(state, client);
	(void)mosquitto_lib_cleanup();
out:
	mqtt_release(state);
	return NULL;
}

int mqtt_start(const struct config_mqtt *settings)
{
	struct mqtt_sender *state;
	sigset_t all;
	sigset_t previous;
	int error;

	if (!settings->enabled)
		return 0;
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return -1;
	state->settings = *settings;
	atomic_init(&state->owners, 2U);
	atomic_init(&state->stopping, false);
	atomic_init(&state->online, false);
	atomic_init(&state->pending, false);
	atomic_init(&state->connecting, false);
	atomic_init(&state->network_stop, false);
	if ((error = pthread_mutex_init(&state->lock, NULL)) != 0) {
		free(state);
		errno = error;
		return -1;
	}
	state->wake_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
	if (state->wake_fd < 0) {
		(void)pthread_mutex_destroy(&state->lock);
		free(state);
		return -1;
	}
	/* Signals belong to the monitor, including while the library creates its network thread. */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	error = pthread_create(&state->thread, NULL, mqtt_thread, state);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	if (error != 0) {
		atomic_store(&state->owners, 1U);
		mqtt_release(state);
		errno = error;
		return -1;
	}
	sender = state;
	return 0;
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

	if (state == NULL || !atomic_load(&state->online))
		return;
	length = strnlen(line, MQTT_LINE_SIZE);
	if (length == MQTT_LINE_SIZE || pthread_mutex_trylock(&state->lock) != 0)
		return;
	if (atomic_load(&state->online) && state->count < MQTT_QUEUE_RECORDS) {
		slot = (state->first + state->count) % MQTT_QUEUE_RECORDS;
		memcpy(state->lines[slot], line, length + 1U);
		state->count++;
	}
	(void)pthread_mutex_unlock(&state->lock);
	(void)eventfd_write(state->wake_fd, 1U);
}

void mqtt_stop(void)
{
	struct mqtt_sender *state = sender;
	const struct timespec step = us_to_timespec(MQTT_STOP_CHECK_US);
	unsigned int check;

	if (state == NULL)
		return;
	sender = NULL;
	atomic_store(&state->stopping, true);
	(void)eventfd_write(state->wake_fd, 1U);
	for (check = 0U; check < MQTT_STOP_US / MQTT_STOP_CHECK_US; check++) {
		if (pthread_tryjoin_np(state->thread, NULL) == 0) {
			mqtt_release(state);
			return;
		}
		(void)clock_nanosleep(CLOCK_MONOTONIC, 0, &step, NULL);
	}
	/* The thread keeps its own reference until it exits; shutdown remains bounded. */
	(void)pthread_detach(state->thread);
	mqtt_release(state);
}
