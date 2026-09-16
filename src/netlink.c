#define _GNU_SOURCE

#include "netlink.h"
#include "error.h"

#include <errno.h>
#include <limits.h>
#include <linux/rtnetlink.h>
#include <netlink/attr.h>
#include <netlink/errno.h>
#include <netlink/handlers.h>
#include <netlink/msg.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>

#define NETLINK_RESPONSE_TIMEOUT_MILLISECONDS 1000

enum response_type {
    RESPONSE_QDISC_DUMP,
    RESPONSE_QDISC_CHANGE
};

struct response_context {
    netlink_message_handler handler;
    void *handler_context;
    enum response_type type;
    int kernel_error;
    bool complete;
    bool parse_failed;
};

static int handle_qdisc_event(struct nl_msg *message, void *context_data);

void netlink_init(struct sqm_mon_netlink *netlink)
{
    *netlink = (struct sqm_mon_netlink) { 0 };
}

int netlink_open(
    struct sqm_mon_netlink *netlink,
    char *error,
    size_t error_size
)
{
    struct nl_sock *socket;
    int result;

    if (netlink->socket != NULL) {
        return 0;
    }

    socket = nl_socket_alloc();
    if (socket == NULL) {
        error_set(error, error_size, "could not allocate rtnetlink socket");
        return -1;
    }

    result = nl_connect(socket, NETLINK_ROUTE);
    if (result < 0) {
        error_set(
            error,
            error_size,
            "could not connect rtnetlink socket: %s",
            nl_geterror(result)
        );
        nl_socket_free(socket);
        return -1;
    }

    /* Requests choose explicitly whether they need an acknowledgement. */
    nl_socket_disable_auto_ack(socket);
    result = nl_socket_set_nonblocking(socket);
    if (result < 0) {
        error_set(
            error,
            error_size,
            "could not make rtnetlink socket nonblocking: %s",
            nl_geterror(result)
        );
        nl_socket_free(socket);
        return -1;
    }

    netlink->socket = socket;
    return 0;
}

void netlink_close_requests(struct sqm_mon_netlink *netlink)
{
    if (netlink->socket != NULL) {
        nl_socket_free(netlink->socket);
        netlink->socket = NULL;
    }
}

void netlink_close(struct sqm_mon_netlink *netlink)
{
    netlink_close_requests(netlink);
    if (netlink->events != NULL) {
        nl_socket_free(netlink->events);
        netlink->events = NULL;
    }
    netlink->event_handler = NULL;
    netlink->event_handler_context = NULL;
    netlink->event_parse_failed = false;
}

int netlink_subscribe_qdiscs(
    struct sqm_mon_netlink *netlink,
    qdisc_event_handler handler,
    void *handler_context,
    char *error,
    size_t error_size
)
{
    struct nl_sock *events;
    int result;

    if (handler == NULL) {
        error_set(error, error_size, "qdisc event handler is null");
        return -1;
    }
    if (netlink->events != NULL) {
        return 0;
    }
    events = nl_socket_alloc();
    if (events == NULL) {
        error_set(error, error_size, "could not allocate qdisc event socket");
        return -1;
    }
    result = nl_connect(events, NETLINK_ROUTE);
    if (result == 0) {
        result = nl_socket_add_memberships(events, RTNLGRP_TC, 0);
    }
    if (result == 0) {
        result = nl_socket_set_nonblocking(events);
    }
    if (result == 0) {
        netlink->event_handler = handler;
        netlink->event_handler_context = handler_context;
        nl_socket_disable_seq_check(events);
        result = nl_socket_modify_cb(
            events,
            NL_CB_VALID,
            NL_CB_CUSTOM,
            handle_qdisc_event,
            netlink
        );
    }
    if (result < 0) {
        error_set(
            error,
            error_size,
            "could not subscribe to qdisc events: %s",
            nl_geterror(result)
        );
        nl_socket_free(events);
        netlink->event_handler = NULL;
        netlink->event_handler_context = NULL;
        return -1;
    }
    netlink->events = events;
    return 0;
}

int netlink_event_descriptor(const struct sqm_mon_netlink *netlink)
{
    return netlink->events == NULL ? -1 : nl_socket_get_fd(netlink->events);
}

static int handle_qdisc_event(struct nl_msg *message, void *context_data)
{
    struct sqm_mon_netlink *netlink = context_data;
    const struct nlmsghdr *header = nlmsg_hdr(message);
    const struct tcmsg *traffic_control;
    struct qdisc_event event;

    if (
        header->nlmsg_type != RTM_NEWQDISC &&
        header->nlmsg_type != RTM_DELQDISC
    ) {
        return NL_OK;
    }
    if (!nlmsg_valid_hdr(header, sizeof(*traffic_control))) {
        netlink->event_parse_failed = true;
        return NL_OK;
    }
    traffic_control = NLMSG_DATA(header);
    event = (struct qdisc_event) {
        .type = header->nlmsg_type == RTM_NEWQDISC
            ? QDISC_CREATED
            : QDISC_REMOVED,
        .interface_index = (unsigned int)traffic_control->tcm_ifindex,
        .handle = traffic_control->tcm_handle,
        .parent = traffic_control->tcm_parent
    };
    if (netlink->event_handler(
            &event,
            netlink->event_handler_context
        ) != 0) {
        netlink->event_parse_failed = true;
    }
    return NL_OK;
}

int netlink_receive_qdisc_events(
    struct sqm_mon_netlink *netlink,
    char *error,
    size_t error_size
)
{
    int result;

    if (
        netlink->events == NULL ||
        netlink->event_handler == NULL
    ) {
        error_set(error, error_size, "qdisc event socket is not open");
        return -1;
    }
    netlink->event_parse_failed = false;
    result = nl_recvmsgs_default(netlink->events);
    if (netlink->event_parse_failed) {
        error_set(error, error_size, "could not parse qdisc event");
        return -1;
    }
    if (result == -NLE_AGAIN || result == -NLE_INTR) {
        return 0;
    }
    if (result < 0) {
        error_set(
            error,
            error_size,
            "could not receive qdisc event: %s",
            nl_geterror(result)
        );
        return -1;
    }
    return 0;
}

static int wait_for_response(
    const struct sqm_mon_netlink *netlink,
    char *error,
    size_t error_size
)
{
    struct pollfd descriptor = {
        .fd = nl_socket_get_fd(netlink->socket),
        .events = POLLIN,
        .revents = 0
    };
    int result;

    do {
        result = poll(
            &descriptor,
            1U,
            NETLINK_RESPONSE_TIMEOUT_MILLISECONDS
        );
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        error_set(
            error,
            error_size,
            "rtnetlink poll failed: %s",
            strerror(errno)
        );
        return -1;
    }
    if (result == 0) {
        error_set(error, error_size, "rtnetlink response timed out");
        return -1;
    }
    if ((descriptor.revents & POLLIN) == 0) {
        error_set(
            error,
            error_size,
            "rtnetlink socket reported an error (revents=0x%x)",
            (unsigned int)descriptor.revents
        );
        return -1;
    }
    return 0;
}

static int handle_valid_response(struct nl_msg *message, void *context_data)
{
    struct response_context *context = context_data;
    const struct nlmsghdr *header = nlmsg_hdr(message);

    if (
        header->nlmsg_type != RTM_NEWQDISC ||
        context->handler == NULL
    ) {
        return NL_OK;
    }
    if (context->handler(header, context->handler_context) != 0) {
        context->parse_failed = true;
    }
    return NL_OK;
}

static int handle_complete_response(
    struct nl_msg *message,
    void *context_data
)
{
    struct response_context *context = context_data;

    (void)message;
    context->complete = true;
    return NL_STOP;
}

static int handle_error_response(
    struct sockaddr_nl *address,
    struct nlmsgerr *netlink_error,
    void *context_data
)
{
    struct response_context *context = context_data;

    (void)address;
    context->kernel_error = netlink_error->error;
    context->complete = true;
    return NL_STOP;
}

static int configure_response_callbacks(
    struct nl_sock *socket,
    struct response_context *context,
    char *error,
    size_t error_size
)
{
    static const struct {
        enum nl_cb_type type;
        nl_recvmsg_msg_cb_t handler;
    } handlers[] = {
        { NL_CB_VALID, handle_valid_response },
        { NL_CB_FINISH, handle_complete_response },
        { NL_CB_ACK, handle_complete_response }
    };
    struct nl_cb *callbacks;
    int result = 0;

    for (size_t index = 0; index < sizeof(handlers) / sizeof(handlers[0]); ++index) {
        result = nl_socket_modify_cb(
            socket,
            handlers[index].type,
            NL_CB_CUSTOM,
            handlers[index].handler,
            context
        );
        if (result < 0) {
            break;
        }
    }
    if (result == 0) {
        callbacks = nl_socket_get_cb(socket);
        result = nl_cb_err(
            callbacks,
            NL_CB_CUSTOM,
            handle_error_response,
            context
        );
        nl_cb_put(callbacks);
    }
    if (result < 0) {
        error_set(
            error,
            error_size,
            "could not configure rtnetlink callbacks: %s",
            nl_geterror(result)
        );
        return -1;
    }
    return 0;
}

static int receive_response(
    struct sqm_mon_netlink *netlink,
    struct response_context *context,
    char *error,
    size_t error_size
)
{
    int result = -1;

    if (configure_response_callbacks(
            netlink->socket,
            context,
            error,
            error_size
        ) != 0) {
        return -1;
    }

    while (!context->complete) {
        if (wait_for_response(netlink, error, error_size) != 0) {
            goto done;
        }

        result = nl_recvmsgs_default(netlink->socket);
        if (context->parse_failed) {
            error_set(error, error_size, "could not parse qdisc response");
            result = -1;
            goto done;
        }
        if (context->kernel_error != 0) {
            error_set(
                error,
                error_size,
                context->type == RESPONSE_QDISC_DUMP
                    ? "qdisc dump failed: %s"
                    : "qdisc change failed: %s",
                strerror(-context->kernel_error)
            );
            result = -1;
            goto done;
        }
        if (result == -NLE_AGAIN || result == -NLE_INTR) {
            continue;
        }
        if (result < 0) {
            error_set(
                error,
                error_size,
                context->type == RESPONSE_QDISC_DUMP
                    ? "could not receive qdisc dump: %s"
                    : "could not receive rtnetlink acknowledgement: %s",
                nl_geterror(result)
            );
            result = -1;
            goto done;
        }
    }

    result = 0;

done:
    return result;
}

static int send_request(
    struct sqm_mon_netlink *netlink,
    struct nl_msg *message,
    enum response_type type,
    char *error,
    size_t error_size
)
{
    int result = nl_send_auto_complete(netlink->socket, message);

    if (result < 0) {
        error_set(
            error,
            error_size,
            type == RESPONSE_QDISC_DUMP
                ? "could not request qdisc dump: %s"
                : "could not send qdisc change: %s",
            nl_geterror(result)
        );
        return -1;
    }
    return 0;
}

int netlink_dump_qdiscs(
    struct sqm_mon_netlink *netlink,
    unsigned int interface_index,
    netlink_message_handler handler,
    void *handler_context,
    char *error,
    size_t error_size
)
{
    struct tcmsg traffic_control = {
        .tcm_family = AF_UNSPEC,
        .tcm_ifindex = (int)interface_index
    };
    struct response_context response = {
        .handler = handler,
        .handler_context = handler_context,
        .type = RESPONSE_QDISC_DUMP
    };
    struct nl_msg *message;
    int result;

    if (netlink->socket == NULL) {
        error_set(error, error_size, "rtnetlink socket is not open");
        return -1;
    }

    message = nlmsg_alloc_simple(RTM_GETQDISC, NLM_F_DUMP);
    if (message == NULL) {
        error_set(error, error_size, "could not allocate qdisc dump request");
        return -1;
    }
    result = nlmsg_append(
        message,
        &traffic_control,
        sizeof(traffic_control),
        NLMSG_ALIGNTO
    );
    if (result < 0) {
        error_set(
            error,
            error_size,
            "could not construct qdisc dump request: %s",
            nl_geterror(result)
        );
        nlmsg_free(message);
        return -1;
    }

    result = send_request(
        netlink,
        message,
        RESPONSE_QDISC_DUMP,
        error,
        error_size
    );
    nlmsg_free(message);
    if (result != 0) {
        return -1;
    }

    return receive_response(netlink, &response, error, error_size);
}

int netlink_change_qdisc_option(
    struct sqm_mon_netlink *netlink,
    unsigned int interface_index,
    uint32_t handle,
    uint32_t parent,
    const char *kind,
    unsigned short option_type,
    const void *option_data,
    size_t option_size,
    char *error,
    size_t error_size
)
{
    struct tcmsg traffic_control = {
        .tcm_family = AF_UNSPEC,
        .tcm_ifindex = (int)interface_index,
        .tcm_handle = handle,
        .tcm_parent = parent
    };
    struct response_context response = {
        .handler = NULL,
        .handler_context = NULL,
        .type = RESPONSE_QDISC_CHANGE
    };
    struct nlattr *options;
    struct nl_msg *message;
    int result;

    if (netlink->socket == NULL) {
        error_set(error, error_size, "rtnetlink socket is not open");
        return -1;
    }
    if (option_size > INT_MAX) {
        error_set(error, error_size, "qdisc option is too large");
        return -1;
    }

    message = nlmsg_alloc_simple(RTM_NEWQDISC, NLM_F_ACK);
    if (message == NULL) {
        error_set(error, error_size, "could not allocate qdisc change request");
        return -1;
    }
    result = nlmsg_append(
        message,
        &traffic_control,
        sizeof(traffic_control),
        NLMSG_ALIGNTO
    );
    if (
        result < 0 ||
        nla_put_string(message, TCA_KIND, kind) < 0
    ) {
        error_set(error, error_size, "could not construct qdisc change request");
        nlmsg_free(message);
        return -1;
    }

    options = nla_nest_start(message, TCA_OPTIONS);
    if (
        options == NULL ||
        nla_put(
            message,
            (int)option_type,
            (int)option_size,
            option_data
        ) < 0
    ) {
        error_set(error, error_size, "qdisc option is too large");
        nlmsg_free(message);
        return -1;
    }
    (void)nla_nest_end(message, options);

    result = send_request(
        netlink,
        message,
        RESPONSE_QDISC_CHANGE,
        error,
        error_size
    );
    nlmsg_free(message);
    if (result != 0) {
        return -1;
    }

    return receive_response(netlink, &response, error, error_size);
}
