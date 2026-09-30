#ifndef CAKE_H_INCLUDED
#define CAKE_H_INCLUDED

#include "common/error.h"
#include "platform/netlink.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cake_observation {
    /* Cached while CAKE is found; zero makes the next read resolve the name. */
    unsigned int interface_index;
    uint32_t handle;
    uint32_t parent;
    uint64_t bandwidth_bits_per_second;
    uint64_t capacity_estimate_bits_per_second;
    uint64_t bytes;
    uint32_t packets;
    uint32_t queue_length;
    uint32_t backlog_bytes;
    uint32_t drops;
    uint32_t memory_limit_bytes;
    uint32_t memory_used_bytes;
    uint32_t mtu_bytes;
    int32_t overhead_bytes;
    uint32_t atm_mode;
    bool raw;
    bool has_bandwidth;
    bool has_basic_stats;
    bool has_mtu;
};

enum cake_read_result {
    CAKE_READ_FOUND,
    CAKE_READ_NOT_FOUND,
    CAKE_READ_ERROR
};

struct cake_read {
    const char *interface;
    /* In: the previous observation (or zeroes), whose index and MTU are reused. */
    struct cake_observation *observation;
    enum cake_read_result result;
    char error[ERROR_SIZE];
    /* Working state owned by cake_read_all(). */
    struct cake_observation previous;
    unsigned int interface_index;
    bool found;
};

/* Fills every read from a single qdisc dump. */
void cake_read_all(
    struct netlink *netlink,
    struct cake_read *reads,
    size_t count
);

/* Pass the previous observation (or zeroes) to reuse its index and MTU. */
enum cake_read_result cake_read(
    struct netlink *netlink,
    const char *interface,
    struct cake_observation *observation,
    char *error,
    size_t error_size
);

/* The observation must come from a successful cake_read(). */
int cake_set_bandwidth(
    struct netlink *netlink,
    const struct cake_observation *observation,
    uint64_t bandwidth_bits_per_second,
    char *error,
    size_t error_size
);

/* Matches pinned cake-autorate's tc-text parser, including PTM/raw fallback. */
uint64_t cake_max_wire_packet_bits(const struct cake_observation *observation);

#endif
