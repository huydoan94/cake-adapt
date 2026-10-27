#ifndef PLATFORM_NETLINK_H_INCLUDED
#define PLATFORM_NETLINK_H_INCLUDED

#include <linux/netlink.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nl_sock;

typedef int (*netlink_message_handler)(const struct nlmsghdr *message, void *context);

/* A qdisc as rtnetlink addresses it. */
struct netlink_qdisc_id {
	unsigned int interface_index;
	uint32_t handle;
	uint32_t parent;
};

static inline bool
qdisc_same(const struct netlink_qdisc_id *first, const struct netlink_qdisc_id *second)
{
	return first->interface_index == second->interface_index &&
	       first->handle == second->handle && first->parent == second->parent;
}

enum netlink_qdisc_event_type {
	NETLINK_QDISC_EVENT_CREATED,
	NETLINK_QDISC_EVENT_REMOVED
};

struct netlink_qdisc_event {
	enum netlink_qdisc_event_type type;
	struct netlink_qdisc_id qdisc;
};

/* One attribute nested in a qdisc's TCA_OPTIONS. */
struct netlink_qdisc_option {
	const char *kind;
	unsigned short type;
	const void *data;
	size_t size;
};

struct netlink;

/* Like a uloop callback: the owner finds itself with container_of(). */
typedef void (*qdisc_event_handler)(
	struct netlink *netlink,
	const struct netlink_qdisc_event *event
);

struct netlink {
	struct nl_sock *socket;
	struct nl_sock *events;
	/* Set by the owner before netlink_subscribe_qdiscs(). */
	qdisc_event_handler netlink_qdisc_event;
	bool event_parse_failed;
};

int netlink_open(struct netlink *netlink, char *error, size_t error_size);

void netlink_close(struct netlink *netlink);
void netlink_close_requests(struct netlink *netlink);

int netlink_subscribe_qdiscs(struct netlink *netlink, char *error, size_t error_size);

int netlink_event_descriptor(const struct netlink *netlink);

int netlink_receive_qdisc_events(struct netlink *netlink, char *error, size_t error_size);

/* Every interface's qdiscs; the kernel does not filter a dump by tcm_ifindex. */
int netlink_dump_qdiscs(
	struct netlink *netlink,
	netlink_message_handler handler,
	void *context,
	char *error,
	size_t error_size
);

int netlink_change_qdisc(
	struct netlink *netlink,
	const struct netlink_qdisc_id *qdisc,
	const struct netlink_qdisc_option *option,
	char *error,
	size_t error_size
);

#endif
