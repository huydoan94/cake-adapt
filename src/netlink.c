#include "netlink.h"
#include "error.h"

#include <errno.h>
#include <limits.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NETLINK_RECEIVE_BUFFER_SIZE 32768U
#define NETLINK_RESPONSE_TIMEOUT_MILLISECONDS 1000

void netlink_init(struct sqm_mon_netlink *netlink)
{
    netlink->socket_descriptor = -1;
    netlink->sequence = 0U;
}

int netlink_open(
    struct sqm_mon_netlink *netlink,
    char *error,
    size_t error_size
)
{
    struct sockaddr_nl local_address = {
        .nl_family = AF_NETLINK,
        .nl_pad = 0U,
        .nl_pid = 0U,
        .nl_groups = 0U
    };
    int descriptor;

    if (netlink->socket_descriptor >= 0) {
        return 0;
    }

    descriptor = socket(
        AF_NETLINK,
        SOCK_RAW | SOCK_CLOEXEC,
        NETLINK_ROUTE
    );
    if (descriptor < 0) {
        error_set(
            error,
            error_size,
            "could not create rtnetlink socket: %s",
            strerror(errno)
        );
        return -1;
    }

    if (bind(
            descriptor,
            (const struct sockaddr *)&local_address,
            sizeof(local_address)
        ) != 0) {
        int saved_errno = errno;

        (void)close(descriptor);
        error_set(
            error,
            error_size,
            "could not bind rtnetlink socket: %s",
            strerror(saved_errno)
        );
        return -1;
    }

    netlink->socket_descriptor = descriptor;
    return 0;
}

void netlink_close(struct sqm_mon_netlink *netlink)
{
    if (netlink->socket_descriptor >= 0) {
        (void)close(netlink->socket_descriptor);
        netlink->socket_descriptor = -1;
    }
}

static uint32_t next_sequence(struct sqm_mon_netlink *netlink)
{
    netlink->sequence++;
    if (netlink->sequence == 0U) {
        netlink->sequence++;
    }
    return netlink->sequence;
}

static int wait_for_response(
    int descriptor,
    char *error,
    size_t error_size
)
{
    struct pollfd poll_descriptor = {
        .fd = descriptor,
        .events = POLLIN,
        .revents = 0
    };
    int poll_result;

    do {
        poll_result = poll(
            &poll_descriptor,
            1U,
            NETLINK_RESPONSE_TIMEOUT_MILLISECONDS
        );
    } while (poll_result < 0 && errno == EINTR);

    if (poll_result < 0) {
        error_set(
            error,
            error_size,
            "rtnetlink poll failed: %s",
            strerror(errno)
        );
        return -1;
    }

    if (poll_result == 0) {
        error_set(error, error_size, "rtnetlink response timed out");
        return -1;
    }

    if ((poll_descriptor.revents & POLLIN) == 0) {
        error_set(
            error,
            error_size,
            "rtnetlink socket reported an error (revents=0x%x)",
            (unsigned int)poll_descriptor.revents
        );
        return -1;
    }

    return 0;
}

static int send_qdisc_request(
    struct sqm_mon_netlink *netlink,
    unsigned int interface_index,
    uint32_t sequence,
    char *error,
    size_t error_size
)
{
    struct {
        struct nlmsghdr header;
        struct tcmsg traffic_control;
    } request = {
        .header = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
            .nlmsg_type = RTM_GETQDISC,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
            .nlmsg_seq = sequence,
            .nlmsg_pid = 0U
        },
        .traffic_control = {
            .tcm_family = AF_UNSPEC,
            .tcm__pad1 = 0U,
            .tcm__pad2 = 0U,
            .tcm_ifindex = (int)interface_index,
            .tcm_handle = 0U,
            .tcm_parent = 0U,
            .tcm_info = 0U
        }
    };
    struct sockaddr_nl kernel_address = {
        .nl_family = AF_NETLINK,
        .nl_pad = 0U,
        .nl_pid = 0U,
        .nl_groups = 0U
    };
    ssize_t bytes_sent;

    bytes_sent = sendto(
        netlink->socket_descriptor,
        &request,
        request.header.nlmsg_len,
        0,
        (const struct sockaddr *)&kernel_address,
        sizeof(kernel_address)
    );
    if (bytes_sent < 0) {
        error_set(
            error,
            error_size,
            "could not request qdisc dump: %s",
            strerror(errno)
        );
        return -1;
    }

    if ((size_t)bytes_sent != request.header.nlmsg_len) {
        error_set(error, error_size, "qdisc dump request was incomplete");
        return -1;
    }

    return 0;
}

static int handle_response_message(
    const struct nlmsghdr *message,
    uint32_t sequence,
    netlink_message_handler handler,
    void *context,
    bool *complete,
    char *error,
    size_t error_size
)
{
    if (message->nlmsg_seq != sequence) {
        return 0;
    }

    switch (message->nlmsg_type) {
    case NLMSG_DONE:
        *complete = true;
        return 0;
    case NLMSG_ERROR:
        if (message->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
            error_set(error, error_size, "received a short rtnetlink error");
            return -1;
        } else {
            const struct nlmsgerr *netlink_error = NLMSG_DATA(message);

            if (netlink_error->error == 0) {
                return 0;
            }

            error_set(
                error,
                error_size,
                "qdisc dump failed: %s",
                strerror(-netlink_error->error)
            );
            return -1;
        }
    case RTM_NEWQDISC:
        if (handler(message, context) != 0) {
            error_set(error, error_size, "could not parse qdisc response");
            return -1;
        }
        return 0;
    default:
        return 0;
    }
}

static int receive_qdisc_response(
    struct sqm_mon_netlink *netlink,
    uint32_t sequence,
    netlink_message_handler handler,
    void *context,
    char *error,
    size_t error_size
)
{
    unsigned char buffer[NETLINK_RECEIVE_BUFFER_SIZE];
    bool complete = false;

    while (!complete) {
        struct sockaddr_nl sender_address;
        struct iovec vector = {
            .iov_base = buffer,
            .iov_len = sizeof(buffer)
        };
        struct msghdr receive_message = {
            .msg_name = &sender_address,
            .msg_namelen = sizeof(sender_address),
            .msg_iov = &vector,
            .msg_iovlen = 1U,
            .msg_control = NULL,
            .msg_controllen = 0U,
            .msg_flags = 0
        };
        const struct nlmsghdr *message;
        ssize_t bytes_received;
        size_t remaining;

        if (wait_for_response(
                netlink->socket_descriptor,
                error,
                error_size
            ) != 0) {
            return -1;
        }

        memset(&sender_address, 0, sizeof(sender_address));
        bytes_received = recvmsg(
            netlink->socket_descriptor,
            &receive_message,
            0
        );
        if (bytes_received < 0) {
            error_set(
                error,
                error_size,
                "could not receive qdisc dump: %s",
                strerror(errno)
            );
            return -1;
        }
        if ((receive_message.msg_flags & MSG_TRUNC) != 0) {
            error_set(error, error_size, "rtnetlink response was truncated");
            return -1;
        }

        if (bytes_received == 0) {
            error_set(error, error_size, "rtnetlink socket closed unexpectedly");
            return -1;
        }

        if (sender_address.nl_pid != 0U) {
            continue;
        }

        if (bytes_received > INT_MAX) {
            error_set(error, error_size, "rtnetlink response was too large");
            return -1;
        }

        remaining = (size_t)bytes_received;
        message = (const struct nlmsghdr *)buffer;
        while (remaining >= sizeof(*message)) {
            size_t aligned_length;

            if (message->nlmsg_len < sizeof(*message) ||
                (size_t)message->nlmsg_len > remaining) {
                error_set(
                    error,
                    error_size,
                    "received malformed rtnetlink data"
                );
                return -1;
            }

            if (handle_response_message(
                    message,
                    sequence,
                    handler,
                    context,
                    &complete,
                    error,
                    error_size
                ) != 0) {
                return -1;
            }

            aligned_length = NLMSG_ALIGN((size_t)message->nlmsg_len);
            if (aligned_length > remaining) {
                error_set(
                    error,
                    error_size,
                    "received malformed rtnetlink alignment"
                );
                return -1;
            }

            remaining -= aligned_length;
            message = (const struct nlmsghdr *)(
                (const unsigned char *)message + aligned_length
            );
        }

        if (remaining != 0) {
            error_set(error, error_size, "received malformed rtnetlink data");
            return -1;
        }
    }

    return 0;
}

int netlink_dump_qdiscs(
    struct sqm_mon_netlink *netlink,
    unsigned int interface_index,
    netlink_message_handler handler,
    void *context,
    char *error,
    size_t error_size
)
{
    uint32_t sequence;

    if (netlink->socket_descriptor < 0) {
        error_set(error, error_size, "rtnetlink socket is not open");
        return -1;
    }

    sequence = next_sequence(netlink);

    if (send_qdisc_request(
            netlink,
            interface_index,
            sequence,
            error,
            error_size
        ) != 0) {
        return -1;
    }

    return receive_qdisc_response(
        netlink,
        sequence,
        handler,
        context,
        error,
        error_size
    );
}

static int append_attribute(
    struct nlmsghdr *header,
    size_t capacity,
    unsigned short type,
    const void *data,
    size_t data_size,
    char *error,
    size_t error_size
)
{
    size_t offset = NLMSG_ALIGN((size_t)header->nlmsg_len);
    size_t attribute_size = RTA_SPACE(data_size);
    struct rtattr *attribute;

    if (offset > capacity || attribute_size > capacity - offset ||
        data_size > UINT16_MAX - RTA_LENGTH(0U)) {
        error_set(error, error_size, "rtnetlink attribute is too large");
        return -1;
    }

    attribute = (struct rtattr *)((unsigned char *)header + offset);
    attribute->rta_type = type;
    attribute->rta_len = (unsigned short)RTA_LENGTH(data_size);
    memcpy(RTA_DATA(attribute), data, data_size);
    header->nlmsg_len = (uint32_t)(offset + attribute_size);
    return 0;
}

static int receive_acknowledgement(
    struct sqm_mon_netlink *netlink,
    uint32_t sequence,
    char *error,
    size_t error_size
)
{
    unsigned char buffer[NETLINK_RECEIVE_BUFFER_SIZE];

    for (;;) {
        struct sockaddr_nl sender_address;
        struct iovec vector = {
            .iov_base = buffer,
            .iov_len = sizeof(buffer)
        };
        struct msghdr receive_message = {
            .msg_name = &sender_address,
            .msg_namelen = sizeof(sender_address),
            .msg_iov = &vector,
            .msg_iovlen = 1U,
            .msg_control = NULL,
            .msg_controllen = 0U,
            .msg_flags = 0
        };
        const struct nlmsghdr *message;
        ssize_t bytes_received;
        size_t remaining;

        if (wait_for_response(
                netlink->socket_descriptor,
                error,
                error_size
            ) != 0) {
            return -1;
        }

        memset(&sender_address, 0, sizeof(sender_address));
        bytes_received = recvmsg(
            netlink->socket_descriptor,
            &receive_message,
            0
        );
        if (bytes_received < 0) {
            error_set(
                error,
                error_size,
                "could not receive rtnetlink acknowledgement: %s",
                strerror(errno)
            );
            return -1;
        }
        if (bytes_received == 0) {
            error_set(
                error,
                error_size,
                "rtnetlink socket closed before acknowledgement"
            );
            return -1;
        }
        if ((receive_message.msg_flags & MSG_TRUNC) != 0) {
            error_set(error, error_size, "rtnetlink acknowledgement was truncated");
            return -1;
        }
        if (sender_address.nl_pid != 0U) {
            continue;
        }

        remaining = (size_t)bytes_received;
        message = (const struct nlmsghdr *)buffer;
        while (remaining >= sizeof(*message)) {
            size_t aligned_length;

            if (message->nlmsg_len < sizeof(*message) ||
                (size_t)message->nlmsg_len > remaining) {
                error_set(
                    error,
                    error_size,
                    "received malformed rtnetlink acknowledgement"
                );
                return -1;
            }
            if (message->nlmsg_seq == sequence &&
                message->nlmsg_type == NLMSG_ERROR) {
                const struct nlmsgerr *netlink_error;

                if (message->nlmsg_len <
                    NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                    error_set(
                        error,
                        error_size,
                        "received a short rtnetlink acknowledgement"
                    );
                    return -1;
                }
                netlink_error = NLMSG_DATA(message);
                if (netlink_error->error == 0) {
                    return 0;
                }
                error_set(
                    error,
                    error_size,
                    "qdisc change failed: %s",
                    strerror(-netlink_error->error)
                );
                return -1;
            }

            aligned_length = NLMSG_ALIGN((size_t)message->nlmsg_len);
            if (aligned_length > remaining) {
                error_set(
                    error,
                    error_size,
                    "received malformed rtnetlink acknowledgement alignment"
                );
                return -1;
            }
            remaining -= aligned_length;
            message = (const struct nlmsghdr *)(
                (const unsigned char *)message + aligned_length
            );
        }
    }
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
    struct {
        struct nlmsghdr header;
        struct tcmsg traffic_control;
        unsigned char attributes[256];
    } request;
    union {
        struct rtattr attribute;
        unsigned char data[256];
    } options;
    struct rtattr *option = &options.attribute;
    struct sockaddr_nl kernel_address = {
        .nl_family = AF_NETLINK,
        .nl_pad = 0U,
        .nl_pid = 0U,
        .nl_groups = 0U
    };
    size_t option_attribute_size = RTA_SPACE(option_size);
    size_t kind_size;
    uint32_t sequence;
    ssize_t bytes_sent;

    if (netlink->socket_descriptor < 0) {
        error_set(error, error_size, "rtnetlink socket is not open");
        return -1;
    }
    if (option_attribute_size > sizeof(options.data) ||
        option_size > UINT16_MAX - RTA_LENGTH(0U)) {
        error_set(error, error_size, "qdisc option is too large");
        return -1;
    }

    memset(&request, 0, sizeof(request));
    memset(&options, 0, sizeof(options));
    sequence = next_sequence(netlink);

    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
    request.header.nlmsg_type = RTM_NEWQDISC;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    request.header.nlmsg_seq = sequence;
    request.traffic_control.tcm_family = AF_UNSPEC;
    request.traffic_control.tcm_ifindex = (int)interface_index;
    request.traffic_control.tcm_handle = handle;
    request.traffic_control.tcm_parent = parent;

    kind_size = strlen(kind) + 1U;
    if (append_attribute(
            &request.header,
            sizeof(request),
            TCA_KIND,
            kind,
            kind_size,
            error,
            error_size
        ) != 0) {
        return -1;
    }

    option->rta_type = option_type;
    option->rta_len = (unsigned short)RTA_LENGTH(option_size);
    memcpy(RTA_DATA(option), option_data, option_size);
    if (append_attribute(
            &request.header,
            sizeof(request),
            TCA_OPTIONS,
            options.data,
            option_attribute_size,
            error,
            error_size
        ) != 0) {
        return -1;
    }

    bytes_sent = sendto(
        netlink->socket_descriptor,
        &request,
        request.header.nlmsg_len,
        0,
        (const struct sockaddr *)&kernel_address,
        sizeof(kernel_address)
    );
    if (bytes_sent < 0) {
        error_set(
            error,
            error_size,
            "could not send qdisc change: %s",
            strerror(errno)
        );
        return -1;
    }
    if ((size_t)bytes_sent != request.header.nlmsg_len) {
        error_set(error, error_size, "qdisc change request was incomplete");
        return -1;
    }

    return receive_acknowledgement(
        netlink,
        sequence,
        error,
        error_size
    );
}
