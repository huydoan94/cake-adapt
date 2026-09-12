#ifndef SQM_MON_NETLINK_H
#define SQM_MON_NETLINK_H

#include <linux/netlink.h>

#include <stddef.h>
#include <stdint.h>

struct sqm_mon_netlink {
    int socket_descriptor;
    uint32_t sequence;
};

typedef int (*netlink_message_handler)(
    const struct nlmsghdr *message,
    void *context
);

void netlink_init(struct sqm_mon_netlink *netlink);

int netlink_open(
    struct sqm_mon_netlink *netlink,
    char *error,
    size_t error_size
);

void netlink_close(struct sqm_mon_netlink *netlink);

int netlink_dump_qdiscs(
    struct sqm_mon_netlink *netlink,
    unsigned int interface_index,
    netlink_message_handler handler,
    void *context,
    char *error,
    size_t error_size
);

#endif
