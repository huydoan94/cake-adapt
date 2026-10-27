#define _GNU_SOURCE

#include "daemon/state.h"

#include "common/arith.h"
#include "common/clock.h"
#include "common/convert.h"
#include "logging/log.h"
#include "constants/latency.h"
#include "constants/text.h"

#include <libubox/utils.h>

#include <errno.h>
#include <string.h>

void native_pinger_unwatch(struct daemon_pingers *pingers)
{
	(void)uloop_timeout_cancel(&pingers->native_timer);
	for (size_t channel = 0U; channel < ARRAY_SIZE(pingers->native_watches); channel++) {
		struct pinger_socket_watch *watch = &pingers->native_watches[channel];

		(void)uloop_fd_delete(&watch->socket);
	}
}

static int schedule_native_tick(struct daemon *daemon, uint64_t now_us)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	struct latency *latency = &pingers->latency;
	uint64_t next_us = native_next_event(&latency->native);

	/* Rounded up, never early: native_tick() sends nothing before the due time. */
	return uloop_timeout_set(
		&pingers->native_timer,
		us_to_timeout_ms(saturating_sub(next_us, now_us))
	);
}

static void handle_native_tick(struct uloop_timeout *timer)
{
	struct daemon *daemon =
		__extension__ container_of(timer, struct daemon, pingers.native_timer);
	struct daemon_pingers *pingers = &daemon->pingers;
	struct latency *latency = &pingers->latency;
	struct native_session *session = &latency->native;
	uint64_t now_us;
	uint64_t realtime_us;
	char error[ERROR_SIZE] = { 0 };

	if (!read_clock_us(CLOCK_MONOTONIC, &now_us) ||
	    !read_clock_us(CLOCK_REALTIME, &realtime_us)) {
		pingers_report_failure(daemon, "native probe clock failed: %s", strerror(errno));
		pingers_defer_retry(daemon);
		return;
	}
	if (native_tick(session, now_us, realtime_us, error, sizeof(error)) != 0)
		pingers_report_degraded(daemon, error);
	else if (!native_send_failed(session))
		daemon_recovered(&pingers->observation_failed, "native probe sends recovered");
	if (!read_clock_us(CLOCK_MONOTONIC, &now_us) || schedule_native_tick(daemon, now_us) != 0) {
		pingers_report_failure(
			daemon,
			"could not schedule native probes: %s",
			strerror(errno)
		);
		pingers_defer_retry(daemon);
	}
}

static void handle_native_reply(struct uloop_fd *descriptor, unsigned int events)
{
	struct pinger_socket_watch *watch =
		__extension__ container_of(descriptor, struct pinger_socket_watch, socket);
	struct daemon *daemon = watch->daemon;
	struct daemon_pingers *pingers = &daemon->pingers;
	struct latency *latency = &pingers->latency;
	struct native_session *session = &latency->native;
	char error[ERROR_SIZE] = { 0 };

	(void)events;
	for (size_t count = 0U; count < NATIVE_RECEIVE_BATCH; count++) {
		struct icmp_reply reply;
		struct latency_sample sample;
		uint64_t now_us;
		uint64_t realtime_us;
		uint64_t response_us;
		enum icmp_receive_result received = icmp_receive(
			&session->channels[watch->channel],
			session->mode,
			&reply,
			error,
			sizeof(error)
		);

		if (received == ICMP_RECEIVE_NONE)
			return;
		if (received == ICMP_RECEIVE_ERROR) {
			pingers_report_degraded(daemon, error);
			pingers_defer_retry(daemon);
			return;
		}
		if (received == ICMP_RECEIVE_IGNORED)
			continue;
		if (!read_clock_us(CLOCK_MONOTONIC, &now_us) ||
		    !read_clock_us(CLOCK_REALTIME, &realtime_us)) {
			pingers_report_failure(
				daemon,
				"native response clock failed: %s",
				strerror(errno)
			);
			pingers_defer_retry(daemon);
			return;
		}
		if (!native_accept(session, &reply, now_us, realtime_us, &sample, &response_us))
			continue;
		if (!reply.kernel_timestamp && !session->timestamp_warned) {
			log_message(
				LOG_LEVEL_WARNING,
				"native receive timestamp unavailable; using read-time clock"
			);
			session->timestamp_warned = true;
		}
		if (!pingers_process_sample(daemon, &sample, &response_us)) {
			pingers_defer_retry(daemon);
			return;
		}
	}
}

bool native_pinger_watch(struct daemon *daemon)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	struct latency *latency = &pingers->latency;
	struct native_session *session = &latency->native;
	struct uloop_timeout *native_timer = &pingers->native_timer;

	for (size_t channel = 0U; channel < ARRAY_SIZE(pingers->native_watches); channel++) {
		struct pinger_socket_watch *watch = &pingers->native_watches[channel];
		struct uloop_fd *reader = &watch->socket;
		struct icmp_socket *channel_socket = &session->channels[channel];
		int descriptor = channel_socket->fd;

		if (descriptor < 0 || reader->registered)
			continue;
		reader->fd = descriptor;
		if (uloop_fd_add(reader, ULOOP_READ) != 0) {
			pingers_report_failure(
				daemon,
				"could not watch native ICMP socket: %s",
				strerror(errno)
			);
			pingers_defer_retry(daemon);
			return false;
		}
	}
	if (!native_timer->pending && uloop_timeout_set(native_timer, 0) != 0) {
		pingers_report_failure(
			daemon,
			"could not start native probe timer: %s",
			strerror(errno)
		);
		pingers_defer_retry(daemon);
		return false;
	}
	return true;
}

void native_pinger_prepare(struct daemon *daemon)
{
	struct daemon_pingers *pingers = &daemon->pingers;
	struct uloop_timeout *native_timer = &pingers->native_timer;

	*native_timer = (struct uloop_timeout){ .cb = handle_native_tick };
	for (size_t channel = 0U; channel < ARRAY_SIZE(pingers->native_watches); channel++) {
		struct pinger_socket_watch *watch = &pingers->native_watches[channel];
		struct uloop_fd *reader = &watch->socket;

		reader->fd = -1;
		reader->cb = handle_native_reply;
		watch->daemon = daemon;
		watch->channel = channel;
	}
}
