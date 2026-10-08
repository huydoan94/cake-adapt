#include "tcpdelay/inject.h"

#include <assert.h>
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
	/* A repeated SYN-ACK is not a new answer. */
	handshake.state = TCPDELAY_INJECT_ANSWERED;
	assert(tcpdelay_inject_classify_answer(&handshake, 1) == TCPDELAY_INJECT_ANSWER_NONE);
	handshake.state = TCPDELAY_INJECT_CLOSED;
	assert(tcpdelay_inject_classify_answer(&handshake, 0) == TCPDELAY_INJECT_ANSWER_NONE);
}

int main(void)
{
	test_server_skipped();
	test_syn();
	test_resets();
	test_answer();
	puts("TCP timestamp injection policy tests passed");
	return 0;
}
