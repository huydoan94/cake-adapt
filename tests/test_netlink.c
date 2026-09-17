#define _GNU_SOURCE

/* Simulate multipart replies and interrupted waits using the actual receiver. */
#define poll test_poll
#define nl_recvmsgs_default test_receive
#define nl_send_auto_complete test_send
#define read_clock_microseconds test_clock
#include "../src/netlink.c"
#undef poll
#undef nl_recvmsgs_default
#undef nl_send_auto_complete
#undef read_clock_microseconds

#include <assert.h>
#include <linux/pkt_sched.h>
#include <stdio.h>

static uint64_t now;
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

bool test_clock(clockid_t clock_identifier, uint64_t *timestamp)
{
    assert(clock_identifier == CLOCK_MONOTONIC);
    *timestamp = now;
    return true;
}

int test_poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
    assert(count == 1U);
    assert(timeout > 0 && timeout <= NETLINK_RESPONSE_TIMEOUT_MILLISECONDS);
    polls++;
    now += 600000U;
    /* Without the shared deadline, a partial reply used to reach this timeout
     * and return the preceding successful receive result. */
    if (polls >= 3U) {
        return 0;
    }
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
    if (acknowledge) {
        return handle_complete_response(NULL, response);
    }
    /* A valid partial dump without NLMSG_DONE. */
    return 0;
}

static void test_response(bool interrupted, bool complete)
{
    struct netlink netlink = { .socket = nl_socket_alloc() };
    struct response_context context = { .type = RESPONSE_QDISC_DUMP };
    char error[128] = "";
    int result;

    assert(netlink.socket != NULL);
    now = 1000000U;
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

static void record_event(const struct qdisc_event *event, void *context)
{
    *(struct qdisc_event *)context = *event;
}

static void test_request(enum response_type type, bool fail_send)
{
    struct netlink netlink = { .socket = nl_socket_alloc() };
    struct response_context context = { .type = type };
    struct nl_msg *message = nlmsg_alloc();
    char error[128] = "";

    assert(netlink.socket != NULL);
    assert(message != NULL);
    now = 1000000U;
    polls = 0U;
    interrupt_wait = false;
    acknowledge = true;
    response = &context;
    send_result = fail_send ? -NLE_FAILURE : 1;
    assert(send_request(&netlink, message, &context, error, sizeof(error)) == (fail_send ? -1 : 0));
    assert(polls == (fail_send ? 0U : 1U));
    if (fail_send) {
        assert(strstr(error, type == RESPONSE_QDISC_DUMP ? "request qdisc dump" : "send qdisc change") != NULL);
    }
    netlink_close(&netlink);
}

static void test_qdisc_events(void)
{
    struct qdisc_event received = { 0 };
    struct netlink netlink = {
        .event_handler = record_event,
        .event_handler_context = &received
    };
    struct tcmsg tc = {
        .tcm_ifindex = 7,
        .tcm_handle = 0x10000,
        .tcm_parent = TC_H_ROOT
    };
    struct nl_msg *message = nlmsg_alloc_simple(RTM_NEWQDISC, 0);

    assert(message != NULL);
    /* Reject truncated kernel payloads before invoking the event handler. */
    assert(handle_qdisc_event(message, &netlink) == NL_OK);
    assert(netlink.event_parse_failed);
    assert(received.interface_index == 0U);
    netlink.event_parse_failed = false;

    assert(nlmsg_append(message, &tc, sizeof(tc), NLMSG_ALIGNTO) == 0);
    assert(handle_qdisc_event(message, &netlink) == NL_OK);
    assert(!netlink.event_parse_failed);
    assert(received.type == QDISC_CREATED);
    assert(received.interface_index == 7U);
    assert(received.handle == tc.tcm_handle);
    assert(received.parent == TC_H_ROOT);

    nlmsg_hdr(message)->nlmsg_type = RTM_DELQDISC;
    assert(handle_qdisc_event(message, &netlink) == NL_OK);
    assert(received.type == QDISC_REMOVED);

    nlmsg_hdr(message)->nlmsg_type = RTM_NEWLINK;
    received.interface_index = 0U;
    assert(handle_qdisc_event(message, &netlink) == NL_OK);
    assert(received.interface_index == 0U);
    assert(!netlink.event_parse_failed);
    nlmsg_free(message);
}

int main(void)
{
    test_response(false, false);
    test_response(true, false);
    test_response(false, true);
    test_qdisc_events();
    test_request(RESPONSE_QDISC_DUMP, false);
    test_request(RESPONSE_QDISC_DUMP, true);
    test_request(RESPONSE_QDISC_CHANGE, false);
    test_request(RESPONSE_QDISC_CHANGE, true);
    puts("netlink request, deadline and event tests passed");
    return 0;
}
