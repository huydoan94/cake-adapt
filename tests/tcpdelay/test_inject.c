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
	handshake.state = TCPDELAY_INJECT_ECHOED;
	assert(tcpdelay_inject_classify_syn(&handshake, 1000U) == TCPDELAY_INJECT_SYN_REJECTED);
	handshake.state = TCPDELAY_INJECT_RETRIED;
	assert(tcpdelay_inject_classify_syn(&handshake, 1000U) == TCPDELAY_INJECT_SYN_PASS);
	handshake.state = TCPDELAY_INJECT_CLOSED;
	assert(tcpdelay_inject_classify_syn(&handshake, 1000U) == TCPDELAY_INJECT_SYN_PASS);
}

/*
 * A client reset right after its SYN, once answered, rejects like a server; a
 * server reset rejects only before any SYN-ACK.
 */
static void test_resets(void)
{
	struct tcpdelay_inject_handshake handshake = {
		.sequence = 0xffffffffU,
		.state = TCPDELAY_INJECT_ECHOED,
	};

	assert(!tcpdelay_inject_client_reset_rejects(NULL, 0U));
	/* Right after the SYN, across the sequence wrap. */
	assert(tcpdelay_inject_client_reset_rejects(&handshake, 0U));
	/* After data: an ordinary close. */
	assert(!tcpdelay_inject_client_reset_rejects(&handshake, 1738U));
	handshake.state = TCPDELAY_INJECT_ECHOED;
	assert(tcpdelay_inject_client_reset_rejects(&handshake, 0U));
	assert(!tcpdelay_inject_server_reset_rejects(&handshake));
	handshake.state = TCPDELAY_INJECT_SENT;
	assert(!tcpdelay_inject_client_reset_rejects(&handshake, 0U));
	assert(tcpdelay_inject_server_reset_rejects(&handshake));
	handshake.state = TCPDELAY_INJECT_RETRIED;
	assert(tcpdelay_inject_server_reset_rejects(&handshake));
	handshake.state = TCPDELAY_INJECT_CLOSED;
	assert(!tcpdelay_inject_client_reset_rejects(&handshake, 0U));
	assert(!tcpdelay_inject_server_reset_rejects(&handshake));
	assert(!tcpdelay_inject_server_reset_rejects(NULL));
}

/* Data acknowledged: past the SYN's sequence + 1, across the sequence wrap. */
static void test_data_acknowledged(void)
{
	assert(!tcpdelay_inject_data_acknowledged(1001U, 1000U));
	assert(tcpdelay_inject_data_acknowledged(1002U, 1000U));
	assert(tcpdelay_inject_data_acknowledged(1449U, 1000U));
	assert(!tcpdelay_inject_data_acknowledged(0U, 0xffffffffU));
	assert(tcpdelay_inject_data_acknowledged(100U, 0xffffffffU));
	/* An older acknowledgement does not count. */
	assert(!tcpdelay_inject_data_acknowledged(900U, 1000U));
}

static void test_answer(void)
{
	struct tcpdelay_inject_handshake handshake = { .state = TCPDELAY_INJECT_SENT };

	assert(tcpdelay_inject_classify_answer(NULL, 1) == TCPDELAY_INJECT_ANSWER_NONE);
	assert(tcpdelay_inject_classify_answer(&handshake, 1) == TCPDELAY_INJECT_ANSWER_ECHOED);
	assert(tcpdelay_inject_classify_answer(&handshake, 0) == TCPDELAY_INJECT_ANSWER_DECLINED);
	handshake.state = TCPDELAY_INJECT_RETRIED;
	assert(tcpdelay_inject_classify_answer(&handshake, 0) == TCPDELAY_INJECT_ANSWER_REJECTED);
	assert(tcpdelay_inject_classify_answer(&handshake, 1) == TCPDELAY_INJECT_ANSWER_ECHOED);
	/* A SYN-ACK again after echoing ours: the server never took the client's ACK. */
	handshake.state = TCPDELAY_INJECT_ECHOED;
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

/*
 * A client gets 1, then 0x80000000 after a stall, and its own learned clock
 * once known; each lasts a day.
 */
static void test_client_tsval(void)
{
	const struct tcpdelay_inject_client learned = { .seen_ns = DAY_NS, .tsval = 2600000000U };
	const struct tcpdelay_inject_client stalled = { .seen_ns = DAY_NS, .stalled = 1U };
	const struct tcpdelay_inject_client both = {
		.seen_ns = DAY_NS,
		.tsval = 2600000000U,
		.stalled = 1U,
	};

	assert(tcpdelay_inject_client_tsval(NULL, DAY_NS) == 1U);
	assert(tcpdelay_inject_client_tsval(&learned, DAY_NS) == 2600000000U);
	assert(tcpdelay_inject_client_tsval(&learned, 2U * DAY_NS - 1U) == 2600000000U);
	assert(tcpdelay_inject_client_tsval(&learned, 2U * DAY_NS) == 1U);
	assert(tcpdelay_inject_client_tsval(&stalled, DAY_NS) == 0x80000000U);
	assert(tcpdelay_inject_client_tsval(&stalled, 2U * DAY_NS) == 1U);
	/* The learned clock wins over a stall. */
	assert(tcpdelay_inject_client_tsval(&both, DAY_NS) == 2600000000U);
}

int main(void)
{
	test_server_skipped();
	test_syn();
	test_resets();
	test_answer();
	test_data_acknowledged();
	test_injected_tsval();
	test_client_tsval();
	puts("TCP timestamp injection policy tests passed");
	return 0;
}
