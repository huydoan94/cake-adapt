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
);

#endif
