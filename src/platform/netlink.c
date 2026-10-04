#define _GNU_SOURCE

#include "platform/netlink.h"
#include "common/constants.h"
#include "config/defaults.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"

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

struct response_context {
	netlink_message_handler handler;
	void *handler_context;
	/* Names the request in errors, such as NETLINK_REQUEST_QDISC_DUMP. */
	const char *request;
	int kernel_error;
	bool complete;
	bool parse_failed;
};

static int handle_qdisc_event(struct nl_msg *message, void *context_data);

/* A connected, nonblocking rtnetlink socket; purpose names it in errors. */
static struct nl_sock *open_socket(const char *purpose, char *error, size_t error_size)
{
	struct nl_sock *socket = nl_socket_alloc();
	int result;

	if (socket == NULL) {
		error_set(error, error_size, "could not allocate %s socket", purpose);
		return NULL;
	}
	result = nl_connect(socket, NETLINK_ROUTE);
	if (result == 0)
		result = nl_socket_set_nonblocking(socket);
	if (result < 0) {
		error_set(
			error,
			error_size,
			"could not open %s socket: %s",
			purpose,
			nl_geterror(result)
		);
		nl_socket_free(socket);
		return NULL;
	}
	return socket;
}

int netlink_open(struct netlink *netlink, char *error, size_t error_size)
{
	if (netlink->socket != NULL)
		return 0;
	netlink->socket = open_socket(NETLINK_SOCKET_REQUESTS, error, error_size);
	if (netlink->socket == NULL)
		return -1;
	/* Requests choose explicitly whether they need an acknowledgement. */
	nl_socket_disable_auto_ack(netlink->socket);
	return 0;
}

/* nl_socket_free() ignores NULL, so both work on sockets that were never opened. */
void netlink_close_requests(struct netlink *netlink)
{
	nl_socket_free(netlink->socket);
	netlink->socket = NULL;
}

void netlink_close(struct netlink *netlink)
{
	nl_socket_free(netlink->socket);
	nl_socket_free(netlink->events);
	*netlink = (struct netlink){ 0 };
}

int netlink_subscribe_qdiscs(
	struct netlink *netlink,
	qdisc_event_handler handler,
	void *handler_context,
	char *error,
	size_t error_size
)
{
	struct nl_sock *events;
	int result;

	if (netlink->events != NULL)
		return 0;
	events = open_socket(NETLINK_SOCKET_EVENTS, error, error_size);
	if (events == NULL)
		return -1;
	nl_socket_disable_seq_check(events);
	result = nl_socket_add_memberships(events, RTNLGRP_TC, 0);
	if (result == 0)
		result = nl_socket_modify_cb(
			events,
			NL_CB_VALID,
			NL_CB_CUSTOM,
			handle_qdisc_event,
			netlink
		);
	if (result < 0) {
		error_set(
			error,
			error_size,
			"could not subscribe to qdisc events: %s",
			nl_geterror(result)
		);
		nl_socket_free(events);
		return -1;
	}
	netlink->events = events;
	netlink->event_handler = handler;
	netlink->event_handler_context = handler_context;
	return 0;
}

int netlink_event_descriptor(const struct netlink *netlink)
{
	return netlink->events == NULL ? -1 : nl_socket_get_fd(netlink->events);
}

static int handle_qdisc_event(struct nl_msg *message, void *context_data)
{
	struct netlink *netlink = context_data;
	const struct nlmsghdr *header = nlmsg_hdr(message);
	const struct tcmsg *traffic_control;
	struct qdisc_event event;

	if (header->nlmsg_type != RTM_NEWQDISC && header->nlmsg_type != RTM_DELQDISC)
		return NL_OK;
	if (!nlmsg_valid_hdr(header, sizeof(*traffic_control))) {
		netlink->event_parse_failed = true;
		return NL_OK;
	}
	traffic_control = NLMSG_DATA(header);
	event = (struct qdisc_event){ .type = header->nlmsg_type == RTM_NEWQDISC ? QDISC_CREATED :
										   QDISC_REMOVED,
				      .interface_index = (unsigned int)traffic_control->tcm_ifindex,
				      .handle = traffic_control->tcm_handle,
				      .parent = traffic_control->tcm_parent };
	netlink->event_handler(&event, netlink->event_handler_context);
	return NL_OK;
}

int netlink_receive_qdisc_events(struct netlink *netlink, char *error, size_t error_size)
{
	int result;

	if (netlink->events == NULL)
		return error_set(error, error_size, "qdisc event socket is not open");
	netlink->event_parse_failed = false;
	result = nl_recvmsgs_default(netlink->events);
	if (netlink->event_parse_failed)
		return error_set(error, error_size, "could not parse qdisc event");
	if (result == -NLE_AGAIN || result == -NLE_INTR)
		return 0;
	if (result < 0) {
		return error_set(
			error,
			error_size,
			"could not receive qdisc event: %s",
			nl_geterror(result)
		);
	}
	return 0;
}

/*
 * One deadline covers a whole multipart response and its interruptions; the
 * first wait sets it when *deadline_microseconds is zero.
 */
static int wait_for_response(
	const struct netlink *netlink,
	uint64_t *deadline_microseconds,
	char *error,
	size_t error_size
)
{
	struct pollfd descriptor = {
		.fd = nl_socket_get_fd(netlink->socket),
		.events = POLLIN,
		.revents = 0,
	};
	int result;

	do {
		uint64_t now;

		if (!read_clock_microseconds(CLOCK_MONOTONIC, &now)) {
			return error_set(
				error,
				error_size,
				"could not read rtnetlink deadline clock: %s",
				strerror(errno)
			);
		}
		if (*deadline_microseconds == 0U) {
			*deadline_microseconds = now + NETLINK_RESPONSE_TIMEOUT_MILLISECONDS *
							       MICROSECONDS_PER_MILLISECOND;
		}
		if (now >= *deadline_microseconds)
			return error_set(error, error_size, "rtnetlink response timed out");
		result = poll(
			&descriptor,
			1U,
			/* The remaining time is within NETLINK_RESPONSE_TIMEOUT_MILLISECONDS. */
			(int)milliseconds_rounded_up(*deadline_microseconds - now)
		);
		/* A poll timeout comes back to the deadline check above. */
	} while ((result < 0 && errno == EINTR) || result == 0);

	if (result < 0)
		return error_set(error, error_size, "rtnetlink poll failed: %s", strerror(errno));
	if ((descriptor.revents & POLLIN) == 0) {
		return error_set(
			error,
			error_size,
			"rtnetlink socket reported an error (revents=0x%x)",
			(unsigned int)descriptor.revents
		);
	}
	return 0;
}

static int handle_valid_response(struct nl_msg *message, void *context_data)
{
	struct response_context *context = context_data;
	const struct nlmsghdr *header = nlmsg_hdr(message);

	if (header->nlmsg_type != RTM_NEWQDISC || context->handler == NULL)
		return NL_OK;
	if (context->handler(header, context->handler_context) != 0)
		context->parse_failed = true;
	return NL_OK;
}

static int handle_complete_response(struct nl_msg *message, void *context_data)
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
		{ NL_CB_ACK, handle_complete_response },
	};
	struct nl_cb *callbacks;
	int result = 0;

	for (size_t index = 0; index < ARRAY_SIZE(handlers); ++index) {
		result = nl_socket_modify_cb(
			socket,
			handlers[index].type,
			NL_CB_CUSTOM,
			handlers[index].handler,
			context
		);
		if (result < 0)
			break;
	}
	if (result == 0) {
		callbacks = nl_socket_get_cb(socket);
		result = nl_cb_err(callbacks, NL_CB_CUSTOM, handle_error_response, context);
		nl_cb_put(callbacks);
	}
	if (result < 0) {
		return error_set(
			error,
			error_size,
			"could not configure rtnetlink callbacks: %s",
			nl_geterror(result)
		);
	}
	return 0;
}

static int receive_response(
	struct netlink *netlink,
	struct response_context *context,
	char *error,
	size_t error_size
)
{
	uint64_t deadline_microseconds = 0U;
	int result;

	if (configure_response_callbacks(netlink->socket, context, error, error_size) != 0)
		return -1;

	while (!context->complete) {
		if (wait_for_response(netlink, &deadline_microseconds, error, error_size) != 0)
			return -1;

		result = nl_recvmsgs_default(netlink->socket);
		if (context->parse_failed)
			return error_set(error, error_size, "could not parse qdisc response");
		if (context->kernel_error != 0) {
			return error_set(
				error,
				error_size,
				"%s failed: %s",
				context->request,
				strerror(-context->kernel_error)
			);
		}
		if (result == -NLE_AGAIN || result == -NLE_INTR)
			continue;
		if (result < 0) {
			return error_set(
				error,
				error_size,
				"could not receive %s response: %s",
				context->request,
				nl_geterror(result)
			);
		}
	}

	return 0;
}

static int send_request(
	struct netlink *netlink,
	struct nl_msg *message,
	struct response_context *response,
	char *error,
	size_t error_size
)
{
	int result;

	if (netlink->socket == NULL) {
		nlmsg_free(message);
		return error_set(error, error_size, "rtnetlink socket is not open");
	}
	result = nl_send_auto_complete(netlink->socket, message);
	/* Consume the request even when sending fails; replies own no message data. */
	nlmsg_free(message);
	if (result < 0) {
		return error_set(
			error,
			error_size,
			"could not send %s request: %s",
			response->request,
			nl_geterror(result)
		);
	}
	return receive_response(netlink, response, error, error_size);
}

int netlink_dump_qdiscs(
	struct netlink *netlink,
	netlink_message_handler handler,
	void *handler_context,
	char *error,
	size_t error_size
)
{
	struct tcmsg traffic_control = { .tcm_family = AF_UNSPEC };
	struct response_context response = {
		.handler = handler,
		.handler_context = handler_context,
		.request = NETLINK_REQUEST_QDISC_DUMP,
	};
	struct nl_msg *message;
	int result;

	message = nlmsg_alloc_simple(RTM_GETQDISC, NLM_F_DUMP);
	if (message == NULL)
		return error_set(error, error_size, "could not allocate qdisc dump request");
	result = nlmsg_append(message, &traffic_control, sizeof(traffic_control), NLMSG_ALIGNTO);
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

	return send_request(netlink, message, &response, error, error_size);
}

int netlink_change_qdisc_option(
	struct netlink *netlink,
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
		.tcm_parent = parent,
	};
	/* A change is only acknowledged, so it needs no message handler. */
	struct response_context response = { .request = NETLINK_REQUEST_QDISC_CHANGE };
	struct nlattr *options;
	struct nl_msg *message;
	int result;

	if (option_size > INT_MAX)
		return error_set(error, error_size, "qdisc option is too large");

	message = nlmsg_alloc_simple(RTM_NEWQDISC, NLM_F_ACK);
	if (message == NULL)
		return error_set(error, error_size, "could not allocate qdisc change request");
	result = nlmsg_append(message, &traffic_control, sizeof(traffic_control), NLMSG_ALIGNTO);
	if (result < 0 || nla_put_string(message, TCA_KIND, kind) < 0) {
		error_set(error, error_size, "could not construct qdisc change request");
		nlmsg_free(message);
		return -1;
	}

	options = nla_nest_start(message, TCA_OPTIONS);
	if (options == NULL ||
	    nla_put(message, (int)option_type, (int)option_size, option_data) < 0) {
		error_set(error, error_size, "qdisc option is too large");
		nlmsg_free(message);
		return -1;
	}
	(void)nla_nest_end(message, options);

	return send_request(netlink, message, &response, error, error_size);
}
