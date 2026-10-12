/* Inspect the queue seam while linking the actual Mosquitto API. */
#include "logging/mqtt.c"

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

int main(void)
{
	const struct config_mqtt disabled = { 0 };

	assert(mqtt_start(&disabled) == 0);
	assert(!mqtt_enabled());
	mqtt_submit("disabled");
	mqtt_stop();
	test_queue();
	(void)puts("MQTT disabled, offline, contention, overflow and payload-boundary tests passed");
	return 0;
}
