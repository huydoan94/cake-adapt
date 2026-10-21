#define _GNU_SOURCE

#include "monitor/loop.h"
#include "logging/log.h"
#include "common/helpers.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/monitor/pingers.c"

static int timer_delay;
static int add_result;
static enum icmp_receive_result receive_result;
static unsigned int receives;
static unsigned int records;
static unsigned int updates;
static unsigned int warnings;
static uint64_t fake_monotonic_us = 10U * SECOND;
static uint64_t fake_realtime_us = 100U * SECOND;
static int64_t clock_step_us;
static uint64_t last_recorded_us;
static uint64_t accepted_age_us;

int __wrap_native_tick(
	struct native_session *session,
	uint64_t now_us,
	uint64_t realtime_us,
	char *error,
	size_t error_size
);

int __wrap_native_tick(
	struct native_session *session,
	uint64_t now_us,
	uint64_t realtime_us,
	char *error,
	size_t error_size
)
{
	(void)realtime_us;
	(void)error;
	(void)error_size;
	fake_monotonic_us = now_us + 2U * MILLISECOND;
	session->next_send_us = fake_monotonic_us + 50U * MILLISECOND;
	return 0;
}

bool __wrap_read_clock_us(clockid_t identifier, uint64_t *timestamp_us);

bool __wrap_read_clock_us(clockid_t identifier, uint64_t *timestamp_us)
{
	*timestamp_us = identifier == CLOCK_MONOTONIC ? fake_monotonic_us : fake_realtime_us;
	return true;
}

int uloop_fd_add(struct uloop_fd *descriptor, unsigned int flags)
{
	assert(flags == ULOOP_READ);
	if (add_result != 0)
		return add_result;
	descriptor->registered = true;
	return 0;
}

int uloop_fd_delete(struct uloop_fd *descriptor)
{
	descriptor->registered = false;
	return 0;
}

int uloop_timeout_set(struct uloop_timeout *timer, int milliseconds)
{
	timer_delay = milliseconds;
	timer->pending = true;
	return 0;
}

int uloop_timeout_cancel(struct uloop_timeout *timer)
{
	timer->pending = false;
	return 0;
}

int uloop_process_add(struct uloop_process *process)
{
	(void)process;
	assert(0 && "native backend must never register a child");
	return -1;
}

void ustream_free(struct ustream *stream)
{
	(void)stream;
	assert(0 && "native backend must never register a stream");
}

void log_message(enum log_level level, const char *format, ...)
{
	(void)format;
	if (level == LOG_LEVEL_WARNING)
		warnings++;
}

size_t reflectors_find(const struct monitor *monitor, const char *target)
{
	(void)monitor;
	assert(strcmp(target, "192.0.2.1") == 0);
	return 0U;
}

void reflectors_record(
	struct monitor *monitor,
	size_t slot,
	const struct latency_sample *sample,
	uint64_t response_us,
	struct latency_observation *observation
)
{
	(void)monitor;
	(void)sample;
	last_recorded_us = response_us;
	assert(slot == 0U);
	*observation = (struct latency_observation){ 0 };
	records++;
}

void control_update(
	struct monitor *monitor,
	const struct latency_observation *observation,
	const struct latency_sample *sample
)
{
	(void)monitor;
	(void)observation;
	(void)sample;
	updates++;
}

enum icmp_receive_result __wrap_icmp_receive(
	const struct icmp_socket *channel,
	enum icmp_mode mode,
	struct icmp_reply *reply,
	char *error,
	size_t error_size
);
void __wrap_icmp_close(struct icmp_socket *channel);
bool __wrap_native_accept(
	struct native_session *session,
	const struct icmp_reply *reply,
	uint64_t now_us,
	uint64_t realtime_us,
	struct latency_sample *sample,
	uint64_t *response_us
);

enum icmp_receive_result __wrap_icmp_receive(
	const struct icmp_socket *channel,
	enum icmp_mode mode,
	struct icmp_reply *reply,
	char *error,
	size_t error_size
)
{
	(void)mode;
	assert(channel->fd == 91);
	receives++;
	*reply = (struct icmp_reply){ 0 };
	if (receive_result == ICMP_RECEIVE_ERROR)
		(void)snprintf(error, error_size, "injected receive failure");
	return receive_result;
}

void __wrap_icmp_close(struct icmp_socket *channel)
{
	assert(!channel->fd || channel->fd == 91 || channel->fd == -1);
	channel->fd = -1;
}

bool __wrap_native_accept(
	struct native_session *session,
	const struct icmp_reply *reply,
	uint64_t now_us,
	uint64_t realtime_us,
	struct latency_sample *sample,
	uint64_t *response_us
)
{
	(void)session;
	(void)reply;
	*sample = (struct latency_sample){ .timestamp_us = realtime_us };
	(void)strcpy(sample->target, "192.0.2.1");
	*response_us = now_us - accepted_age_us;
	/* Inject a wall-clock step after native acceptance but before monitor recording. */
	if (clock_step_us != 0) {
		fake_realtime_us = (uint64_t)((int64_t)fake_realtime_us + clock_step_us);
		clock_step_us = 0;
	}
	return true;
}

static void initialize(struct monitor *monitor, const struct config *config)
{
	const struct latency_settings settings = { .pinger_method = PINGER_METHOD_NATIVE };

	struct monitor_pingers *pingers = &monitor->pingers;
	struct latency *latency = &pingers->latency;
	struct native_session *session = &latency->native;
	struct icmp_socket *channel_socket = &session->channels[NATIVE_CHANNEL_IPV4];
	struct native_watch *watch = &pingers->native_watches[NATIVE_CHANNEL_IPV4];

	memset(monitor, 0, sizeof(*monitor));
	monitor->config = config;
	latency_init(latency, &settings);
	latency->active = true;
	channel_socket->fd = 91;
	watch->monitor = monitor;
	watch->channel = 0U;
}

int main(void)
{
	static struct monitor monitor;
	const struct config config = { .interface_up_check_interval_us = 10U * SECOND };

	initialize(&monitor, &config);
	handle_native_tick(&monitor.pingers.native_timer);
	assert(timer_delay == 50);
	monitor.pingers.latency.native.next_send_us = SECOND + 1U;
	assert(schedule_native_tick(&monitor, SECOND) == 0 && timer_delay == 1);
	assert(watch_native_session(&monitor));
	assert(monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket.registered);
	assert(!monitor.pingers.native_watches[NATIVE_CHANNEL_IPV6].socket.registered);
	assert(monitor.pingers.native_timer.pending);
	pingers_close(&monitor);
	assert(!monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket.registered);
	assert(!monitor.pingers.native_timer.pending);
	assert(!monitor.pingers.latency.active);
	assert(monitor.pingers.latency.native.channels[NATIVE_CHANNEL_IPV4].fd == -1);
	initialize(&monitor, &config);
	add_result = -1;
	assert(!watch_native_session(&monitor));
	assert(!monitor.pingers.latency.active && monitor.pingers.next_attempt_us > 0U);
	add_result = 0;
	initialize(&monitor, &config);
	receive_result = ICMP_RECEIVE_IGNORED;
	receives = 0U;
	handle_native_reply(&monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket, ULOOP_READ);
	assert(receives == 64U && records == 0U && updates == 0U);
	receive_result = ICMP_RECEIVE_REPLY;
	unsigned int before = warnings;
	handle_native_reply(&monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket, ULOOP_READ);
	assert(records == 64U && updates == 64U && warnings == before + 1U);
	handle_native_reply(&monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket, ULOOP_READ);
	assert(records == 128U && warnings == before + 1U);
	clock_step_us = (int64_t)SECOND;
	handle_native_reply(&monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket, ULOOP_READ);
	assert(records == 192U && updates == 192U && last_recorded_us == fake_monotonic_us);
	clock_step_us = -(int64_t)SECOND;
	handle_native_reply(&monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket, ULOOP_READ);
	assert(records == 256U && updates == 256U && last_recorded_us == fake_monotonic_us);
	accepted_age_us = LATENCY_STALE_RESPONSE_US + 1U;
	handle_native_reply(&monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket, ULOOP_READ);
	assert(records == 320U && updates == 256U);
	assert(last_recorded_us == fake_monotonic_us - accepted_age_us);
	receive_result = ICMP_RECEIVE_ERROR;
	handle_native_reply(&monitor.pingers.native_watches[NATIVE_CHANNEL_IPV4].socket, ULOOP_READ);
	assert(!monitor.pingers.latency.active && monitor.pingers.next_attempt_us > 0U);
	puts("native pinger monitor tests passed");
	return 0;
}
