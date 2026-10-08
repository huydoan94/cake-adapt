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

/* Minutes in microseconds, written out so the checks do not reuse the constants. */
#define MINUTE_US 60000000ULL

static void test_breaker(void)
{
	struct tcpdelay_inject_breaker breaker = { 0 };
	uint64_t now = 1000U * MINUTE_US;

	/* Two stalls since loading, then a third ten minutes later: still running. */
	assert(tcpdelay_inject_breaker_check(&breaker, 2U, now) == TCPDELAY_INJECT_BREAKER_KEEP);
	now += 11U * MINUTE_US;
	assert(tcpdelay_inject_breaker_check(&breaker, 3U, now) == TCPDELAY_INJECT_BREAKER_KEEP);
	/* Three more within ten minutes pause it for a day. */
	now += 5U * MINUTE_US;
	assert(tcpdelay_inject_breaker_check(&breaker, 5U, now) == TCPDELAY_INJECT_BREAKER_KEEP);
	now += 4U * MINUTE_US;
	assert(tcpdelay_inject_breaker_check(&breaker, 6U, now) == TCPDELAY_INJECT_BREAKER_PAUSE);
	assert(breaker.paused);
	now += 60U * MINUTE_US;
	assert(tcpdelay_inject_breaker_check(&breaker, 6U, now) == TCPDELAY_INJECT_BREAKER_KEEP);
	/* A day after pausing it runs again, counting from the counter at that check. */
	now += 23U * 60U * MINUTE_US;
	assert(tcpdelay_inject_breaker_check(&breaker, 6U, now) == TCPDELAY_INJECT_BREAKER_RESUME);
	assert(!breaker.paused);
	now += MINUTE_US;
	assert(tcpdelay_inject_breaker_check(&breaker, 8U, now) == TCPDELAY_INJECT_BREAKER_KEEP);
	now += MINUTE_US;
	assert(tcpdelay_inject_breaker_check(&breaker, 9U, now) == TCPDELAY_INJECT_BREAKER_PAUSE);
}

static void test_breaker_first_check(void)
{
	struct tcpdelay_inject_breaker breaker = { 0 };

	/* Three stalls before the first check, counted from zero at loading. */
	assert(tcpdelay_inject_breaker_check(&breaker, 3U, MINUTE_US) ==
	       TCPDELAY_INJECT_BREAKER_PAUSE);
}

static void test_breaker_history(void)
{
	struct tcpdelay_inject_breaker breaker = { 0 };
	uint64_t now = MINUTE_US;
	uint64_t check;

	/* One stall every six minutes, for longer than the history holds: never three in ten. */
	for (check = 0U; check < 40U; check++) {
		assert(tcpdelay_inject_breaker_check(&breaker, check, now) ==
		       TCPDELAY_INJECT_BREAKER_KEEP);
		now += 6U * MINUTE_US;
	}
}

int main(void)
{
	test_server_skipped();
	test_syn();
	test_resets();
	test_answer();
	test_injected_tsval();
	test_breaker();
	test_breaker_first_check();
	test_breaker_history();
	puts("TCP timestamp injection policy tests passed");
	return 0;
}
