#ifndef CAKE_H_INCLUDED
#define CAKE_H_INCLUDED

#include "netlink.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cake_observation {
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
    bool has_bandwidth;
    bool has_basic_stats;
};

enum cake_read_result {
    CAKE_READ_FOUND,
    CAKE_READ_NOT_FOUND,
    CAKE_READ_ERROR
};

enum cake_read_result cake_read(
    struct netlink *netlink,
    const char *interface,
    struct cake_observation *observation,
    char *error,
    size_t error_size
);

int cake_set_bandwidth(
    struct netlink *netlink,
    const char *interface,
    const struct cake_observation *observation,
    uint64_t bandwidth_bits_per_second,
    char *error,
    size_t error_size
);

#endif
