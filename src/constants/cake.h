#ifndef CONSTANTS_CAKE_H_INCLUDED
#define CONSTANTS_CAKE_H_INCLUDED

/* CAKE, its qdiscs, netlink requests and packet accounting. */

#include "constants/units.h"

#define QDISC_KIND "cake"
/* Multi-queue CAKE: one cake per transmit queue under a root that holds the shared settings. */
#define QDISC_KIND_MQ "cake_mq"
#define IFB_PREFIX "ifb4"

/* CAKE holds its rate in whole bytes/s. */
#define SHAPER_RATE_STEP_BPS BITS_PER_BYTE
/* Longest "<rate> bit/s": UINT64_MAX has 20 digits. */
#define CAKE_BANDWIDTH_TEXT_SIZE 32U
/* As cake-autorate: a sample covers at least ten maximum-size packets each way. */
#define CAKE_COMPENSATION_PACKET_TIMES 10U

/* ATM sends 48 payload bytes in each 53-byte cell; PTM adds a byte per 64. */
#define CAKE_ATM_CELL_PAYLOAD_BYTES 48U
#define CAKE_ATM_CELL_BYTES 53U
#define CAKE_PTM_BLOCK_BYTES 64U

/* rtnetlink sockets and requests, as errors name them. */
#define NETLINK_SOCKET_REQUESTS "rtnetlink"
#define NETLINK_SOCKET_EVENTS "qdisc event"
#define NETLINK_REQUEST_QDISC_DUMP "qdisc dump"
#define NETLINK_REQUEST_QDISC_CHANGE "qdisc change"
#define NETLINK_RESPONSE_TIMEOUT_US SECOND

#endif
