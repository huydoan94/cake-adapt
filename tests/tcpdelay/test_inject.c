#include "tcpdelay/inject.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* 24 hours in nanoseconds, written out so the check does not reuse the constant. */
#define DAY_NS 86400000000000ULL

static void test_server_skipped(void)
{
	const struct tcpdelay_inject_server server = { .rejected_ns = 5000000000ULL };

	assert(!tcpdelay_inject_server_skipped(NULL, 1U));
	assert(tcpdelay_inject_server_skipped(&server, server.rejected_ns));
	assert(tcpdelay_inject_server_skipped(&server, server.rejected_ns + DAY_NS - 1U));
	/* After a day the server is tried again. */
	assert(!tcpdelay_inject_server_skipped(&server, server.rejected_ns + DAY_NS));
}

static void test_syn(void)
{
	struct tcpdelay_inject_handshake handshake = {
		.sequence = 1000U,
		.state = TCPDELAY_INJECT_SENT,
	};

	assert(tcpdelay_inject_classify_syn(NULL, 1000U) == TCPDELAY_INJECT_SYN_NEW);
	/* The same 4-tuple with another sequence is a new connection. */
	assert(tcpdelay_inject_classify_syn(&handshake, 2000U) == TCPDELAY_INJECT_SYN_NEW);
	assert(tcpdelay_inject_classify_syn(&handshake, 1000U) == TCPDELAY_INJECT_SYN_RETRY);
	handshake.state = TCPDELAY_INJECT_ANSWERED;
	assert(tcpdelay_inject_classify_syn(&handshake, 1000U) == TCPDELAY_INJECT_SYN_REJECTED);
	handshake.state = TCPDELAY_INJECT_ACCEPTED;
	assert(tcpdelay_inject_classify_syn(&handshake, 1000U) == TCPDELAY_INJECT_SYN_REJECTED);
	handshake.state = TCPDELAY_INJECT_RETRIED;
	assert(tcpdelay_inject_classify_syn(&handshake, 1000U) == TCPDELAY_INJECT_SYN_PASS);
	handshake.state = TCPDELAY_INJECT_CLOSED;
	assert(tcpdelay_inject_classify_syn(&handshake, 1000U) == TCPDELAY_INJECT_SYN_PASS);
}

static void test_resets(void)
{
	struct tcpdelay_inject_handshake handshake = {
		.sequence = 0xffffffffU,
		.state = TCPDELAY_INJECT_ANSWERED,
	};

	assert(!tcpdelay_inject_client_reset_rejects(NULL, 0U));
	/* Right after the SYN, across the sequence wrap. */
	assert(tcpdelay_inject_client_reset_rejects(&handshake, 0U));
	/* After data: an ordinary close. */
	assert(!tcpdelay_inject_client_reset_rejects(&handshake, 1738U));
	handshake.state = TCPDELAY_INJECT_ACCEPTED;
	assert(tcpdelay_inject_client_reset_rejects(&handshake, 0U));
	assert(!tcpdelay_inject_server_reset_rejects(&handshake));
	handshake.state = TCPDELAY_INJECT_SENT;
	assert(!tcpdelay_inject_client_reset_rejects(&handshake, 0U));
	assert(tcpdelay_inject_server_reset_rejects(&handshake));
	handshake.state = TCPDELAY_INJECT_RETRIED;
	assert(tcpdelay_inject_server_reset_rejects(&handshake));
	handshake.state = TCPDELAY_INJECT_ANSWERED;
	assert(!tcpdelay_inject_server_reset_rejects(&handshake));
	assert(!tcpdelay_inject_server_reset_rejects(NULL));
}

static void test_answer(void)
{
	struct tcpdelay_inject_handshake handshake = { .state = TCPDELAY_INJECT_SENT };

	assert(tcpdelay_inject_classify_answer(NULL, 1) == TCPDELAY_INJECT_ANSWER_NONE);
	assert(tcpdelay_inject_classify_answer(&handshake, 1) == TCPDELAY_INJECT_ANSWER_ACCEPTED);
	assert(tcpdelay_inject_classify_answer(&handshake, 0) == TCPDELAY_INJECT_ANSWER_DECLINED);
	handshake.state = TCPDELAY_INJECT_RETRIED;
	assert(tcpdelay_inject_classify_answer(&handshake, 0) == TCPDELAY_INJECT_ANSWER_REJECTED);
	assert(tcpdelay_inject_classify_answer(&handshake, 1) == TCPDELAY_INJECT_ANSWER_ACCEPTED);
	/* Without timestamps a repeated SYN-ACK is not a new answer... */
	handshake.state = TCPDELAY_INJECT_ANSWERED;
	assert(tcpdelay_inject_classify_answer(&handshake, 1) == TCPDELAY_INJECT_ANSWER_NONE);
	assert(tcpdelay_inject_classify_answer(&handshake, 0) == TCPDELAY_INJECT_ANSWER_NONE);
	/* ...with ours, the server never took the client's ACK. */
	handshake.state = TCPDELAY_INJECT_ACCEPTED;
	assert(tcpdelay_inject_classify_answer(&handshake, 1) == TCPDELAY_INJECT_ANSWER_STALLED);
	assert(tcpdelay_inject_classify_answer(&handshake, 0) == TCPDELAY_INJECT_ANSWER_STALLED);
	handshake.state = TCPDELAY_INJECT_CLOSED;
	assert(tcpdelay_inject_classify_answer(&handshake, 0) == TCPDELAY_INJECT_ANSWER_NONE);
}

/*
 * The client's later TSvals must not look older than the injected one: a
 * server compares them by their signed 32-bit difference. 2^31 ms is 24.8
 * days of Windows uptime.
 */
static void test_injected_tsval(void)
{
	const uint32_t injected = TCPDELAY_INJECT_TSVAL;

	assert(injected == 1U);
	/* The first millisecond after boot, and the last one before 24.8 days. */
	assert((int32_t)(1U - injected) >= 0);
	assert((int32_t)(0x80000000U - injected) >= 0);
	/* 35439554 ms, the Windows client seen on 2026-10-07. */
	assert((int32_t)(35439554U - injected) >= 0);
	/* Beyond 24.8 days the client looks older, and the server drops it. */
	assert((int32_t)(0x80000001U - injected) < 0);
}

/* Seconds and days in nanoseconds, written out so the checks do not reuse the constants. */
#define SECOND_NS 1000000000ULL
#define DAY_TICKS 86400000U

/* Five stalls within ten seconds make a burst; spread out, they do not. */
static void test_stall_burst(void)
{
	struct tcpdelay_inject_state state = { 0 };
	uint64_t now = 1000U * SECOND_NS;
	unsigned int i;

	for (i = 0U; i < 4U; i++)
		assert(!tcpdelay_inject_stall_burst(&state, now + i * SECOND_NS));
	assert(tcpdelay_inject_stall_burst(&state, now + 9U * SECOND_NS));
	/* One every three seconds: never five within ten. */
	state = (struct tcpdelay_inject_state){ 0 };
	for (i = 0U; i < 20U; i++)
		assert(!tcpdelay_inject_stall_burst(&state, now + i * 3U * SECOND_NS));
}

/*
 * After a burst: switch to the youngest clock learned within a day when it is
 * at least a day old; a second burst, or no such clock, pauses for a day.
 */
static void test_escalate(void)
{
	struct tcpdelay_inject_state state = { 0 };
	uint64_t now = 10U * DAY_NS;

	assert(tcpdelay_inject_ipv4_tsval(&state) == 1U);
	/* Clients up 3 and 30 days; the 3-day one is the youngest. */
	tcpdelay_inject_learn_clock(&state, 3U * DAY_TICKS, now);
	tcpdelay_inject_learn_clock(&state, 30U * DAY_TICKS, now);
	assert(tcpdelay_inject_youngest_clock(&state, now) == 3U * DAY_TICKS);
	/* A minute later every clock reads a minute more. */
	assert(tcpdelay_inject_youngest_clock(&state, now + 60U * SECOND_NS) ==
	       3U * DAY_TICKS + 60000U);
	/* A burst stops IPv4 injection until the daemon resolves it. */
	state.burst = 1U;
	assert(tcpdelay_inject_ipv4_tsval(&state) == 0U);
	assert(tcpdelay_inject_resolve(&state, now) == TCPDELAY_INJECT_TO_SWITCHED);
	assert(state.mode == TCPDELAY_INJECT_SWITCHED && !state.burst);
	/* Ten minutes of margin below the youngest clock. */
	assert(tcpdelay_inject_ipv4_tsval(&state) == 3U * DAY_TICKS - 600000U);
	assert(tcpdelay_inject_resolve(&state, now) == TCPDELAY_INJECT_UNCHANGED);
	state.burst = 1U;
	assert(tcpdelay_inject_resolve(&state, now) == TCPDELAY_INJECT_TO_PAUSED);
	assert(tcpdelay_inject_ipv4_tsval(&state) == 0U);
	assert(tcpdelay_inject_resolve(&state, now + DAY_NS - 1U) == TCPDELAY_INJECT_UNCHANGED);
	/* A day later it starts over with 1. */
	assert(tcpdelay_inject_resolve(&state, now + DAY_NS) == TCPDELAY_INJECT_TO_NORMAL);
	assert(tcpdelay_inject_ipv4_tsval(&state) == 1U);
	assert(state.mode == TCPDELAY_INJECT_NORMAL);
}

static void test_escalate_young(void)
{
	struct tcpdelay_inject_state state = { 0 };
	uint64_t now = 10U * DAY_NS;

	/* A client up 2 hours: younger than a day, so a burst pauses. */
	tcpdelay_inject_learn_clock(&state, 7200000U, now);
	tcpdelay_inject_learn_clock(&state, 30U * DAY_TICKS, now);
	assert(tcpdelay_inject_escalate(&state, now));
	assert(state.mode == TCPDELAY_INJECT_PAUSED);
	/* Nothing learned at all also pauses. */
	state = (struct tcpdelay_inject_state){ 0 };
	assert(tcpdelay_inject_escalate(&state, now));
	/* A burst with nothing learned pauses at once. */
	state = (struct tcpdelay_inject_state){ .burst = 1U };
	assert(tcpdelay_inject_resolve(&state, now) == TCPDELAY_INJECT_TO_PAUSED);
	assert(state.mode == TCPDELAY_INJECT_PAUSED && !state.burst);
	/* Clocks older than a day are forgotten. */
	state = (struct tcpdelay_inject_state){ 0 };
	tcpdelay_inject_learn_clock(&state, 5U * DAY_TICKS, now);
	assert(tcpdelay_inject_youngest_clock(&state, now + DAY_NS) == 0U);
}

/* An IPv6 client gets its own learned clock for a day, else 1. */
static void test_client_tsval(void)
{
	const struct tcpdelay_inject_client client = { .seen_ns = DAY_NS, .tsval = 2600000000U };

	assert(tcpdelay_inject_client_tsval(NULL, DAY_NS) == 1U);
	assert(tcpdelay_inject_client_tsval(&client, DAY_NS) == 2600000000U);
	assert(tcpdelay_inject_client_tsval(&client, 2U * DAY_NS - 1U) == 2600000000U);
	assert(tcpdelay_inject_client_tsval(&client, 2U * DAY_NS) == 1U);
}

int main(void)
{
	test_server_skipped();
	test_syn();
	test_resets();
	test_answer();
	test_injected_tsval();
	test_stall_burst();
	test_escalate();
	test_escalate_young();
	test_client_tsval();
	puts("TCP timestamp injection policy tests passed");
	return 0;
}
