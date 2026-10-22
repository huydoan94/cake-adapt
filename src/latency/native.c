#define _GNU_SOURCE

#include "latency/native.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "config/defaults.h"

#include <errno.h>
#include <inttypes.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

/* ICMP timestamps count milliseconds since UTC midnight. */
#define DAY_MS (24U * 60U * MINUTE / MILLISECOND)

static uint32_t midnight_ms(uint64_t realtime_us)
{
	/* Divide the 64-bit epoch first: us_to_ms() saturates, for uloop durations. */
	return (uint32_t)(realtime_us / MILLISECOND % DAY_MS);
}

static enum native_channel native_channel(int family)
{
	return family == AF_INET ? NATIVE_CHANNEL_IPV4 : NATIVE_CHANNEL_IPV6;
}

void native_init(struct native_session *session)
{
	*session = (struct native_session){ .channels = { { .fd = -1 }, { .fd = -1 } } };
}

bool native_send_failed(const struct native_session *session)
{
	for (size_t index = 0U; index < session->target_count; index++) {
		const struct native_target *target = &session->targets[index];

		if (target->send_failed)
			return true;
	}
	return false;
}

void native_close(struct native_session *session)
{
	for (size_t index = 0U; index < ARRAY_SIZE(session->channels); index++)
		icmp_close(&session->channels[index]);
	free(session->requests);
	native_init(session);
}

int native_open(
	struct native_session *session,
	enum icmp_mode mode,
	const char *interface,
	const char *const *targets,
	size_t target_count,
	uint64_t interval_us,
	uint64_t now_us,
	char *error,
	size_t error_size
)
{
	uint16_t identifier;
	uint64_t period_ms = rounded_divide(interval_us, MICROSECONDS_PER_MILLISECOND);
	uint64_t spacing_ms;

	if (session->requests != NULL || target_count == 0U ||
	    target_count > CONFIG_MAX_REFLECTORS || interval_us / target_count < MILLISECOND)
		return error_set(error, error_size, "invalid native probe session");
	session->mode = mode;
	spacing_ms = rounded_divide(interval_us, target_count * MICROSECONDS_PER_MILLISECOND);
	/* Same millisecond rounding as fping -p and -i. Never exceed its send cadence. */
	session->spacing_us = spacing_ms * MILLISECOND;
	session->period_us = max_u64(period_ms * MILLISECOND, session->spacing_us * target_count);
	session->capacity = (size_t)(LATENCY_RESPONSE_TIMEOUT_US / session->spacing_us + 2U);
	session->requests = calloc(session->capacity, sizeof(*session->requests));
	if (session->requests == NULL) {
		(void)error_set(error, error_size, "could not allocate native probe requests");
		goto failed;
	}
	if (getrandom(&session->token, sizeof(session->token), GRND_NONBLOCK) !=
		    (ssize_t)sizeof(session->token) ||
	    getrandom(&identifier, sizeof(identifier), GRND_NONBLOCK) !=
		    (ssize_t)sizeof(identifier)) {
		(void)error_set(
			error,
			error_size,
			"native probe randomness unavailable: %s",
			strerror(errno)
		);
		goto failed;
	}
	for (size_t index = 0U; index < target_count; index++) {
		struct native_target *target = &session->targets[index];
		struct icmp_address *target_address = &target->address;
		struct sockaddr_storage *target_storage = &target_address->address;
		struct icmp_socket *channel_socket;
		int family;

		target->name = targets[index];
		target->next_send_us = saturating_add(now_us, index * session->spacing_us);
		if (icmp_address_parse(&target->address, target->name) != 0) {
			(void)error_set(
				error,
				error_size,
				"native reflector '%s' must be a numeric IP address",
				target->name
			);
			goto failed;
		}
		family = target_storage->ss_family;
		if (family == AF_INET6) {
			struct sockaddr_in6 *address = (struct sockaddr_in6 *)target_storage;

			if (IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr))
				address->sin6_scope_id = if_nametoindex(interface);
		}
		channel_socket = &session->channels[native_channel(family)];
		if (channel_socket->fd >= 0)
			continue;
		if (icmp_open(
			    channel_socket,
			    family,
			    session->mode,
			    interface,
			    identifier,
			    error,
			    error_size
		    ) != 0)
			goto failed;
	}
	session->target_count = target_count;
	session->next_send_us = now_us;
	return 0;
failed:
	native_close(session);
	return -1;
}

/* The ring slot a request serial uses; it outlives the response timeout. */
static struct native_request *request_slot(const struct native_session *session, uint64_t serial)
{
	return &session->requests[serial % session->capacity];
}

/* When an unanswered request times out. */
static uint64_t request_deadline_us(const struct native_request *request)
{
	return saturating_add(request->sent_us, LATENCY_RESPONSE_TIMEOUT_US);
}

static void native_expire(struct native_session *session, uint64_t now_us)
{
	while (session->expired_serial < session->next_serial) {
		struct native_request *request = request_slot(session, session->expired_serial);

		if (now_us < request_deadline_us(request))
			break;
		if (request->pending) {
			session->timeout_count++;
			request->pending = false;
		}
		session->expired_serial++;
	}
}

uint64_t native_next_event(const struct native_session *session)
{
	uint64_t next_us = session->next_send_us;

	if (session->expired_serial < session->next_serial) {
		const struct native_request *request =
			request_slot(session, session->expired_serial);

		next_us = min_u64(next_us, request_deadline_us(request));
	}
	return next_us;
}

int native_tick(
	struct native_session *session,
	uint64_t now_us,
	uint64_t realtime_us,
	char *error,
	size_t error_size
)
{
	struct native_target *target;
	const struct native_target *next_target;
	const struct icmp_address *target_address;
	const struct sockaddr_storage *target_storage;
	struct native_request *request;
	uint64_t due_us;

	native_expire(session, now_us);
	if (now_us < session->next_send_us)
		return 0;
	target = &session->targets[session->next_target];
	if (now_us < target->next_send_us) {
		session->next_send_us = target->next_send_us;
		return 0;
	}
	request = request_slot(session, session->next_serial);
	*request = (struct native_request){ .serial = session->next_serial,
					    .sent_us = now_us,
					    .originate_ms = midnight_ms(realtime_us),
					    .target = session->next_target };
	target_address = &target->address;
	target_storage = &target_address->address;
	target->send_failed = icmp_send(
				      &session->channels[native_channel(target_storage->ss_family)],
				      &target->address,
				      session->mode,
				      (uint16_t)session->next_serial,
				      session->token,
				      session->next_serial,
				      request->originate_ms,
				      &request->sent_us,
				      error,
				      error_size
			      ) != 0;
	request->pending = !target->send_failed;
	if (target->send_failed)
		session->timeout_count++;
	/*
	 * Schedule from the due time, so timer lateness does not stretch the
	 * period. A send a whole spacing late restarts the schedule from now
	 * instead of catching up in a burst.
	 */
	due_us = session->next_send_us;
	if (now_us - due_us >= session->spacing_us)
		due_us = now_us;
	target->next_send_us = saturating_add(due_us, session->period_us);
	session->next_serial++;
	session->next_target = (session->next_target + 1U) % session->target_count;
	next_target = &session->targets[session->next_target];
	session->next_send_us =
		max_u64(saturating_add(due_us, session->spacing_us), next_target->next_send_us);
	return target->send_failed ? 1 : 0;
}

bool native_accept(
	struct native_session *session,
	const struct icmp_reply *reply,
	uint64_t now_us,
	uint64_t realtime_us,
	struct latency_sample *sample,
	uint64_t *response_us
)
{
	const bool timestamps = session->mode == ICMP_MODE_TIMESTAMP;
	struct native_request *request;
	const struct native_target *target;
	uint64_t serial;
	uint64_t received_us;
	uint64_t age_us;

	if (session->next_serial == 0U)
		return false;
	/* At least 1ms between requests, and 10s timeout: no live 16-bit sequence alias. */
	serial = session->next_serial - 1U;
	age_us = (uint16_t)((uint16_t)serial - reply->sequence);
	if (age_us > serial)
		return false;
	serial -= age_us;
	request = request_slot(session, serial);
	if (!request->pending || request->serial != serial ||
	    now_us >= request_deadline_us(request))
		return false;
	target = &session->targets[request->target];
	if (!icmp_address_equal(&target->address, &reply->source))
		return false;
	/* Echo replies carry the session token and full serial; timestamps only the originate time. */
	if (timestamps ? reply->originate_ms != request->originate_ms :
			 (reply->token != session->token || reply->serial != serial))
		return false;
	/* Translate receive realtime to monotonic once, retaining queueing delay in the socket. */
	received_us = response_monotonic_us(realtime_us, now_us, reply->realtime_us);
	if (received_us < request->sent_us || reply->realtime_us > realtime_us)
		return false;
	*sample = (struct latency_sample){ .timestamp_us = reply->realtime_us,
					   .sequence = serial / session->target_count,
					   .timestamp_rollover_sensitive = timestamps };
	(void)snprintf(sample->target, sizeof(sample->target), "%s", target->name);
	(void)snprintf(
		sample->timestamp_text,
		sizeof(sample->timestamp_text),
		"[%" PRIu64 ".%06" PRIu64 "]",
		reply->realtime_us / SECOND,
		reply->realtime_us % SECOND
	);
	if (timestamps) {
		uint32_t arrival_ms = midnight_ms(reply->realtime_us);

		/* Preserve fping-ts's signed fields, including clock offset and midnight rollover. */
		sample->upload_owd_us =
			((int64_t)reply->receive_ms - request->originate_ms) * (int64_t)MILLISECOND;
		sample->download_owd_us =
			((int64_t)arrival_ms - reply->transmit_ms) * (int64_t)MILLISECOND;
	} else {
		sample->download_owd_us =
			(int64_t)rounded_divide(received_us - request->sent_us, 2U);
		sample->upload_owd_us = sample->download_owd_us;
	}
	request->pending = false;
	*response_us = received_us;
	return true;
}
