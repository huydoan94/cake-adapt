/* Inspect the queue seam while linking the actual Mosquitto API. */
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
#include "logging/mqtt.c"
#if MQTT_TLS_SUPPORTED
#undef mosquitto_tls_set
#endif

#include <assert.h>
#include <stdio.h>

static void test_queue(void)
{
	struct mqtt_sender state = { 0 };
	char boundary[MQTT_LINE_SIZE + 1U];
	unsigned int index;

	assert(pthread_mutex_init(&state.lock, NULL) == 0);
	state.wake_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
	assert(state.wake_fd >= 0);
	atomic_init(&state.online, false);
	atomic_init(&state.pending, false);
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
	mqtt_send(&state, NULL);
	assert(state.count == 0U && !atomic_load(&state.online));
	sender = NULL;
	(void)close(state.wake_fd);
	(void)pthread_mutex_destroy(&state.lock);
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
	assert(state.client == NULL);
	assert(!atomic_load(&state.online));
	assert(mosquitto_lib_cleanup() == MOSQ_ERR_SUCCESS);
#else
	assert(mqtt_start(&state.settings) == -1);
	assert(errno == ENOTSUP);
	assert(!mqtt_enabled());
#endif
}

int main(void)
{
	const struct config_mqtt disabled = { 0 };

	assert(mqtt_start(&disabled) == 0);
	assert(!mqtt_enabled());
	mqtt_submit("disabled");
	mqtt_stop();
	test_queue();
	test_tls();
	(void)puts("MQTT queue, disabled and TLS setup tests passed");
	return 0;
}
