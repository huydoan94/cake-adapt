#ifndef NETLINK_H_INCLUDED
#define NETLINK_H_INCLUDED

#include <linux/netlink.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nl_sock;

typedef int (*netlink_message_handler)(
    const struct nlmsghdr *message,
    void *context
);

enum qdisc_event_type {
    QDISC_CREATED,
    QDISC_REMOVED
};

struct qdisc_event {
    enum qdisc_event_type type;
    unsigned int interface_index;
    uint32_t handle;
    uint32_t parent;
};

typedef void (*qdisc_event_handler)(
    const struct qdisc_event *event,
    void *context
);

struct netlink {
    struct nl_sock *socket;
    struct nl_sock *events;
    qdisc_event_handler event_handler;
    void *event_handler_context;
    bool event_parse_failed;
};

void netlink_init(struct netlink *netlink);

int netlink_open(
    struct netlink *netlink,
    char *error,
    size_t error_size
);

void netlink_close(struct netlink *netlink);
void netlink_close_requests(struct netlink *netlink);

int netlink_subscribe_qdiscs(
    struct netlink *netlink,
    qdisc_event_handler handler,
    void *handler_context,
    char *error,
    size_t error_size
);

int netlink_event_descriptor(const struct netlink *netlink);

int netlink_receive_qdisc_events(
    struct netlink *netlink,
    char *error,
    size_t error_size
);

int netlink_dump_qdiscs(
    struct netlink *netlink,
    unsigned int interface_index,
    netlink_message_handler handler,
    void *context,
    char *error,
    size_t error_size
);

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
);

#endif
