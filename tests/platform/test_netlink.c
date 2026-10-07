#define _GNU_SOURCE

/* Simulate multipart replies and interrupted waits using the actual receiver. */
#define poll test_poll
#define nl_recvmsgs_default test_receive
#define nl_send_auto_complete test_send
#define read_clock_us test_clock
#include "platform/netlink.c"
#undef poll
#undef nl_recvmsgs_default
#undef nl_send_auto_complete
#undef read_clock_us

#include <assert.h>
#include <linux/pkt_sched.h>
#include <stdio.h>

static uint64_t now_us;
static unsigned int polls;
static bool interrupt_wait;
static bool acknowledge;
static struct response_context *response;
static int send_result;

int test_send(struct nl_sock *socket, struct nl_msg *message)
{
	assert(socket != NULL);
	assert(message != NULL);
	return send_result;
}

bool test_clock(clockid_t clock_identifier, uint64_t *timestamp_us)
{
	assert(clock_identifier == CLOCK_MONOTONIC);
	*timestamp_us = now_us;
	return true;
}

int test_poll(struct pollfd *descriptors, nfds_t count, int timeout_ms)
{
	assert(count == 1U);
	assert(timeout_ms > 0 &&
	       (uint64_t)timeout_ms <= NETLINK_RESPONSE_TIMEOUT_US / MICROSECONDS_PER_MILLISECOND);
	polls++;
	now_us += 600U * MILLISECOND;
	/* Without the shared deadline, a partial reply used to reach this timeout
	 * and return the preceding successful receive result. */
	if (polls >= 3U)
		return 0;
	if (interrupt_wait) {
		errno = EINTR;
		return -1;
	}
	descriptors[0].revents = POLLIN;
	return 1;
}

int test_receive(struct nl_sock *socket)
{
	assert(socket != NULL);
	if (acknowledge)
		return handle_complete_response(NULL, response);
	/* A valid partial dump without NLMSG_DONE. */
	return 0;
}

static void test_response(bool interrupted, bool complete)
{
	struct netlink netlink = { .socket = nl_socket_alloc() };
	struct response_context context = { .request = NETLINK_REQUEST_QDISC_DUMP };
	char error[128] = "";
	int result;

	assert(netlink.socket != NULL);
	now_us = SECOND;
	polls = 0U;
	interrupt_wait = interrupted;
	acknowledge = complete;
	response = &context;
	result = receive_response(&netlink, &context, error, sizeof(error));
	if (complete) {
		assert(result == 0);
		assert(polls == 1U);
	} else {
		assert(result == -1);
		assert(strstr(error, "timed out") != NULL);
		assert(polls == 2U);
	}
	netlink_close(&netlink);
}

static struct qdisc_event received;

static void record_event(struct netlink *netlink, const struct qdisc_event *event)
{
	(void)netlink;
	received = *event;
}

static void test_request(const char *request, bool fail_send)
{
	struct netlink netlink = { .socket = nl_socket_alloc() };
	struct response_context context = { .request = request };
	struct nl_msg *message = nlmsg_alloc();
	char error[128] = "";

	assert(netlink.socket != NULL);
	assert(message != NULL);
	now_us = SECOND;
	polls = 0U;
	interrupt_wait = false;
	acknowledge = true;
	response = &context;
	send_result = fail_send ? -NLE_FAILURE : 1;
	assert(send_request(&netlink, message, &context, error, sizeof(error)) ==
	       (fail_send ? -1 : 0));
	assert(polls == (fail_send ? 0U : 1U));
	if (fail_send)
		assert(strstr(error, request) != NULL);
	netlink_close(&netlink);
}

static void test_qdisc_events(void)
{
	struct netlink netlink = { .qdisc_event = record_event };
	struct tcmsg tc = { .tcm_ifindex = 7, .tcm_handle = 0x10000, .tcm_parent = TC_H_ROOT };
	struct nl_msg *message = nlmsg_alloc_simple(RTM_NEWQDISC, 0);

	assert(message != NULL);
	/* Reject truncated kernel payloads before invoking the event handler. */
	assert(handle_qdisc_event(message, &netlink) == NL_OK);
	assert(netlink.event_parse_failed);
	assert(received.qdisc.interface_index == 0U);
	netlink.event_parse_failed = false;

	assert(nlmsg_append(message, &tc, sizeof(tc), NLMSG_ALIGNTO) == 0);
	assert(handle_qdisc_event(message, &netlink) == NL_OK);
	assert(!netlink.event_parse_failed);
	assert(received.type == QDISC_CREATED);
	assert(received.qdisc.interface_index == 7U);
	assert(received.qdisc.handle == tc.tcm_handle);
	assert(received.qdisc.parent == TC_H_ROOT);

	nlmsg_hdr(message)->nlmsg_type = RTM_DELQDISC;
	assert(handle_qdisc_event(message, &netlink) == NL_OK);
	assert(received.type == QDISC_REMOVED);

	nlmsg_hdr(message)->nlmsg_type = RTM_NEWLINK;
	received.qdisc.interface_index = 0U;
	assert(handle_qdisc_event(message, &netlink) == NL_OK);
	assert(received.qdisc.interface_index == 0U);
	assert(!netlink.event_parse_failed);
	nlmsg_free(message);
}

int main(void)
{
	test_response(false, false);
	test_response(true, false);
	test_response(false, true);
	test_qdisc_events();
	test_request(NETLINK_REQUEST_QDISC_DUMP, false);
	test_request(NETLINK_REQUEST_QDISC_DUMP, true);
	test_request(NETLINK_REQUEST_QDISC_CHANGE, false);
	test_request(NETLINK_REQUEST_QDISC_CHANGE, true);
	puts("netlink request, deadline and event tests passed");
	return 0;
}
