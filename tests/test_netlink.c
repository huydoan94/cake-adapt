#define _GNU_SOURCE

/* Simulate multipart replies and interrupted waits using the actual receiver. */
#define poll test_poll
#define nl_recvmsgs_default test_receive
#define read_clock_microseconds test_clock
#include "../src/netlink.c"
#undef poll
#undef nl_recvmsgs_default
#undef read_clock_microseconds

#include <assert.h>
#include <stdio.h>

static uint64_t now;
static unsigned int polls;
static bool interrupt_wait;
static bool acknowledge;
static struct response_context *response;

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

int main(void)
{
    test_response(false, false);
    test_response(true, false);
    test_response(false, true);
    puts("netlink deadline tests passed");
    return 0;
}
