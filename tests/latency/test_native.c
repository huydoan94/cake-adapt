#define _GNU_SOURCE

#include "latency/native.h"
#include "config/defaults.h"
#include "constants/latency.h"
#include "constants/units.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int __wrap_icmp_open(
	struct icmp_socket *channel,
	int family,
	enum icmp_mode mode,
	const char *interface,
	uint16_t identifier,
	char *error,
	size_t error_size
);
void __wrap_icmp_close(struct icmp_socket *channel);
int __wrap_icmp_send(
	const struct icmp_socket *channel,
	const struct icmp_address *address,
	enum icmp_mode mode,
	uint16_t sequence,
	uint64_t token,
	uint64_t serial,
	uint32_t originate_ms,
	uint64_t *sent_us,
	char *error,
	size_t error_size
);

static unsigned int opens;
static unsigned int closes;
static unsigned int sends;
static bool fail_open;
static bool fail_send;
static uint64_t send_delay_us;
static uint32_t sent_originate_ms;

int __wrap_icmp_open(
	struct icmp_socket *channel,
	int family,
	enum icmp_mode mode,
	const char *interface,
	uint16_t identifier,
	char *error,
	size_t error_size
)
{
	(void)mode;
	(void)error;
	(void)error_size;
	assert(strcmp(interface, "lo") == 0);
	if (fail_open)
		return -1;
	*channel = (struct icmp_socket){ .fd = 99, .family = family, .identifier = identifier };
	opens++;
	return 0;
}

void __wrap_icmp_close(struct icmp_socket *channel)
{
	if (channel->fd >= 0)
		closes++;
	channel->fd = -1;
}

int __wrap_icmp_send(
	const struct icmp_socket *channel,
	const struct icmp_address *address,
	enum icmp_mode mode,
	uint16_t sequence,
	uint64_t token,
	uint64_t serial,
	uint32_t originate_ms,
	uint64_t *sent_us,
	char *error,
	size_t error_size
)
{
	(void)address;
	(void)mode;
	(void)token;
	sent_originate_ms = originate_ms;
	(void)error;
	(void)error_size;
	assert(channel->fd >= 0);
	assert(sequence == (uint16_t)serial);
	if (fail_send)
		return -1;
	*sent_us += send_delay_us;
	sends++;
	return 0;
}

static void open_session(struct native_session *session, enum icmp_mode mode, uint64_t now_us)
{
	const char *targets[] = { "192.0.2.1", "192.0.2.2" };
	const struct icmp_socket *ipv4 = &session->channels[NATIVE_CHANNEL_IPV4];
	const struct icmp_socket *ipv6 = &session->channels[NATIVE_CHANNEL_IPV6];
	char error[256] = "";

	native_init(session);
	assert(native_open(
		       session,
		       mode,
		       "lo",
		       targets,
		       2U,
		       100U * MILLISECOND,
		       now_us,
		       error,
		       sizeof(error)
	       ) == 0);
	assert(ipv4->fd == 99 && ipv6->fd == -1);
}

static struct icmp_reply
reply_for(struct native_session *session, uint64_t serial, uint64_t realtime_us)
{
	struct native_request *request = &session->requests[serial % session->capacity];
	const struct native_target *target = &session->targets[request->target];

	return (struct icmp_reply){ .source = target->address,
				    .sequence = (uint16_t)serial,
				    .token = session->token,
				    .serial = serial,
				    .originate_ms = request->originate_ms,
				    .realtime_us = realtime_us };
}

static void test_schedule(void)
{
	struct native_session session;
	char error[256] = "";
	uint64_t start_us = SECOND;
	unsigned int before = sends;

	open_session(&session, ICMP_MODE_ECHO, start_us);
	assert(native_next_event(&session) == start_us);
	assert(native_tick(&session, start_us, 2U * SECOND, error, sizeof(error)) == 0);
	assert(sends == before + 1U);
	assert(native_next_event(&session) == start_us + 50U * MILLISECOND);
	assert(native_tick(&session, start_us + MILLISECOND, 2U * SECOND, error, sizeof(error)) ==
	       0);
	assert(sends == before + 1U);
	/* A late event sends once and schedules from now, never catches up as a burst. */
	assert(native_tick(&session, start_us + SECOND, 3U * SECOND, error, sizeof(error)) == 0);
	assert(sends == before + 2U);
	assert(native_next_event(&session) == start_us + SECOND + 50U * MILLISECOND);
	assert(native_tick(
		       &session,
		       start_us + LATENCY_RESPONSE_TIMEOUT_US,
		       12U * SECOND,
		       error,
		       sizeof(error)
	       ) == 0);
	assert(session.timeout_count == 1U);
	assert(!session.requests[0].pending);
	native_close(&session);
	assert(session.requests == NULL && session.channels[NATIVE_CHANNEL_IPV4].fd == -1);
	native_close(&session);
}

/* Late timer callbacks within the spacing do not stretch each target's period. */
static void test_late_callbacks(void)
{
	struct native_session session;
	char error[256] = "";
	uint64_t now_us = SECOND;
	uint64_t first_us[2] = { 0U, 0U };
	uint64_t last_us[2] = { 0U, 0U };
	unsigned int before = sends;

	open_session(&session, ICMP_MODE_ECHO, now_us);
	while (session.next_serial < 200U) {
		uint64_t serial = session.next_serial;
		const struct native_request *request;

		/* Every callback 6 ms late: the testbed's p95 lateness. */
		now_us = native_next_event(&session) + 6U * MILLISECOND;
		assert(native_tick(&session, now_us, 2U * SECOND, error, sizeof(error)) == 0);
		if (session.next_serial == serial)
			continue;
		request = &session.requests[serial % session.capacity];
		if (serial < 2U)
			first_us[request->target] = request->sent_us;
		last_us[request->target] = request->sent_us;
	}
	assert(sends == before + 200U);
	/* 100 sends per target: 99 periods of exactly 100 ms. */
	for (size_t target = 0U; target < 2U; target++)
		assert(last_us[target] - first_us[target] == 99U * 100U * MILLISECOND);
	native_close(&session);
}

static void test_send_timestamp(void)
{
	struct native_session session;
	struct latency_sample sample;
	uint64_t response_us;
	char error[256] = "";

	open_session(&session, ICMP_MODE_ECHO, SECOND);
	send_delay_us = 2U * MILLISECOND;
	assert(native_tick(&session, SECOND, 2U * SECOND, error, sizeof(error)) == 0);
	assert(session.requests[0].sent_us == SECOND + 2U * MILLISECOND);
	/* The schedule follows the due time, not the slower send. */
	assert(native_next_event(&session) == SECOND + 50U * MILLISECOND);
	struct icmp_reply reply = reply_for(&session, 0U, 2U * SECOND + 10U * MILLISECOND);
	assert(native_accept(
		&session,
		&reply,
		SECOND + 10U * MILLISECOND,
		2U * SECOND + 10U * MILLISECOND,
		&sample,
		&response_us
	));
	assert(sample.download_owd_us == 4000 && sample.upload_owd_us == 4000);
	send_delay_us = 0U;
	native_close(&session);
}

static void test_matching(void)
{
	struct native_session session;
	struct latency_sample sample;
	struct icmp_reply reply;
	uint64_t response_us;
	char error[256] = "";

	open_session(&session, ICMP_MODE_ECHO, SECOND);
	assert(native_tick(&session, SECOND, 2U * SECOND, error, sizeof(error)) == 0);
	reply = reply_for(&session, 0U, 2U * SECOND + 10U * MILLISECOND);
	reply.source = session.targets[1].address;
	assert(!native_accept(
		&session,
		&reply,
		SECOND + 20U * MILLISECOND,
		2U * SECOND + 20U * MILLISECOND,
		&sample,
		&response_us
	));
	reply.source = session.targets[0].address;
	reply.token ^= 1U;
	assert(!native_accept(
		&session,
		&reply,
		SECOND + 20U * MILLISECOND,
		2U * SECOND + 20U * MILLISECOND,
		&sample,
		&response_us
	));
	reply.token = session.token;
	reply.serial = 65536U;
	assert(!native_accept(
		&session,
		&reply,
		SECOND + 20U * MILLISECOND,
		2U * SECOND + 20U * MILLISECOND,
		&sample,
		&response_us
	));
	reply.serial = 0U;
	assert(native_accept(
		&session,
		&reply,
		SECOND + 20U * MILLISECOND,
		2U * SECOND + 20U * MILLISECOND,
		&sample,
		&response_us
	));
	assert(response_us == SECOND + 10U * MILLISECOND);
	assert(sample.download_owd_us == 5000 && sample.upload_owd_us == 5000);
	assert(strcmp(sample.timestamp_text, "[2.010000]") == 0);
	assert(sample.sequence == 0U && !sample.timestamp_rollover_sensitive);
	assert(!native_accept(
		&session,
		&reply,
		SECOND + 20U * MILLISECOND,
		2U * SECOND + 20U * MILLISECOND,
		&sample,
		&response_us
	));
	assert(native_tick(
		       &session,
		       SECOND + 50U * MILLISECOND,
		       2U * SECOND + 50U * MILLISECOND,
		       error,
		       sizeof(error)
	       ) == 0);
	reply = reply_for(&session, 1U, 2U * SECOND + 60U * MILLISECOND);
	assert(!native_accept(
		&session,
		&reply,
		SECOND + 50U * MILLISECOND + LATENCY_RESPONSE_TIMEOUT_US,
		2U * SECOND + 50U * MILLISECOND + LATENCY_RESPONSE_TIMEOUT_US,
		&sample,
		&response_us
	));
	native_close(&session);
}

static void test_wrap_and_capacity(void)
{
	struct native_session session;
	struct latency_sample sample;
	uint64_t response_us;
	char error[256] = "";
	struct icmp_reply reply;

	open_session(&session, ICMP_MODE_ECHO, SECOND);
	for (uint64_t serial = 0U; serial < 65538U; serial++) {
		uint64_t now_us = SECOND + serial * 50U * MILLISECOND;
		uint64_t realtime_us = 10U * SECOND + now_us;

		assert(native_tick(&session, now_us, realtime_us, error, sizeof(error)) == 0);
		assert(session.requests[serial % session.capacity].serial == serial);
		reply = reply_for(&session, serial, realtime_us + MILLISECOND);
		assert(native_accept(
			&session,
			&reply,
			now_us + MILLISECOND,
			realtime_us + MILLISECOND,
			&sample,
			&response_us
		));
		assert(sample.sequence == serial / 2U);
	}
	assert(session.timeout_count == 0U);
	native_close(&session);
}

static void test_timestamps(void)
{
	struct native_session session;
	struct latency_sample sample;
	struct icmp_reply reply;
	uint64_t response_us;
	char error[256] = "";
	uint64_t midnight_us = 24U * 60U * MINUTE;

	open_session(&session, ICMP_MODE_TIMESTAMP, SECOND);
	assert(native_tick(&session, SECOND, midnight_us - MILLISECOND, error, sizeof(error)) == 0);
	reply = reply_for(&session, 0U, midnight_us + 2U * MILLISECOND);
	reply.receive_ms = 0U;
	reply.transmit_ms = 1U;
	reply.originate_ms--;
	assert(!native_accept(
		&session,
		&reply,
		SECOND + 3U * MILLISECOND,
		midnight_us + 2U * MILLISECOND,
		&sample,
		&response_us
	));
	reply.originate_ms++;
	assert(native_accept(
		&session,
		&reply,
		SECOND + 3U * MILLISECOND,
		midnight_us + 2U * MILLISECOND,
		&sample,
		&response_us
	));
	/* Like fping-ts, signed midnight offset is retained and tracker sees rollover. */
	assert(sample.upload_owd_us == -((int64_t)midnight_us - (int64_t)MILLISECOND));
	assert(sample.download_owd_us == 1000 && sample.timestamp_rollover_sensitive);
	native_close(&session);
}

static void test_epoch_timestamps(void)
{
	struct native_session session;
	struct latency_sample sample;
	struct icmp_reply reply;
	uint64_t response_us;
	char error[256] = "";
	/* 2026-10-13 20:23:59.809999 UTC: beyond the uloop timer's 32-bit limit. */
	uint64_t realtime_us = UINT64_C(1791923039809999);

	open_session(&session, ICMP_MODE_TIMESTAMP, SECOND);
	assert(native_tick(&session, SECOND, realtime_us, error, sizeof(error)) == 0);
	assert(sent_originate_ms == 73439809U);
	reply = reply_for(&session, 0U, realtime_us + 25U * MILLISECOND);
	reply.receive_ms = 73439820U;
	reply.transmit_ms = 73439822U;
	assert(native_accept(
		&session,
		&reply,
		SECOND + 25U * MILLISECOND,
		realtime_us + 25U * MILLISECOND,
		&sample,
		&response_us
	));
	assert(sample.upload_owd_us == 11 * (int64_t)MILLISECOND);
	assert(sample.download_owd_us == 12 * (int64_t)MILLISECOND);
	assert(sample.timestamp_rollover_sensitive);
	native_close(&session);
}

static void test_failure_cleanup(void)
{
	struct native_session session;
	const char *targets[] = { "192.0.2.1", "bad.example" };
	char error[256] = "";
	unsigned int before = closes;

	native_init(&session);
	assert(native_open(
		       &session,
		       ICMP_MODE_ECHO,
		       "lo",
		       targets,
		       2U,
		       SECOND,
		       SECOND,
		       error,
		       sizeof(error)
	       ) == -1);
	assert(strstr(error, "numeric IP address") != NULL);
	assert(closes == before + 1U && session.requests == NULL);
	fail_open = true;
	assert(native_open(
		       &session,
		       ICMP_MODE_ECHO,
		       "lo",
		       targets,
		       1U,
		       SECOND,
		       SECOND,
		       error,
		       sizeof(error)
	       ) == -1);
	assert(session.requests == NULL);
	fail_open = false;
	open_session(&session, ICMP_MODE_ECHO, SECOND);
	fail_send = true;
	assert(native_tick(&session, SECOND, 2U * SECOND, error, sizeof(error)) == 1);
	assert(session.next_serial == 1U && !session.requests[0].pending);
	assert(native_send_failed(&session) && session.timeout_count == 1U);
	fail_send = false;
	assert(native_tick(
		       &session,
		       SECOND + 50U * MILLISECOND,
		       2U * SECOND + 50U * MILLISECOND,
		       error,
		       sizeof(error)
	       ) == 0);
	assert(session.next_serial == 2U && session.requests[1].pending);
	struct icmp_reply reply = reply_for(&session, 1U, 2U * SECOND + 60U * MILLISECOND);
	struct latency_sample sample;
	uint64_t response_us;
	assert(native_accept(
		&session,
		&reply,
		SECOND + 60U * MILLISECOND,
		2U * SECOND + 60U * MILLISECOND,
		&sample,
		&response_us
	));
	assert(strcmp(sample.target, "192.0.2.2") == 0);
	assert(native_send_failed(&session));
	assert(native_tick(
		       &session,
		       SECOND + 100U * MILLISECOND,
		       2U * SECOND + 100U * MILLISECOND,
		       error,
		       sizeof(error)
	       ) == 0);
	assert(!native_send_failed(&session));
	native_close(&session);
	assert(opens == closes);
}

int main(void)
{
	test_schedule();
	test_late_callbacks();
	test_send_timestamp();
	test_matching();
	test_wrap_and_capacity();
	test_timestamps();
	test_epoch_timestamps();
	test_failure_cleanup();
	puts("native probe session tests passed");
	return 0;
}
