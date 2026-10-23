#ifndef CONSTANTS_ICMP_H_INCLUDED
#define CONSTANTS_ICMP_H_INCLUDED

/* Native ICMP probes: packet sizes and kernel socket options. */

#include "constants/units.h"

/* Linux's fixed time64 socket option; older musl headers expose only SO_TIMESTAMPNS. */
#ifndef SO_TIMESTAMPNS_NEW
#define SO_TIMESTAMPNS_NEW 64
#endif
/* linux/icmp.h's raw-socket type filter, at SOL_RAW; that header conflicts with netinet's. */
#define LINUX_ICMP_FILTER 1

/* Request sizes: echo carries the session token and serial in 56 data bytes. */
#define ICMP_ECHO_REQUEST_BYTES 64U
#define ICMP_TIMESTAMP_REQUEST_BYTES 20U
/* Type, code, checksum, identifier, sequence, then the token and serial. */
#define ICMP_ECHO_REPLY_MIN_BYTES 24U
/* Room for any reply: larger datagrams are foreign, and MSG_TRUNC drops them. */
#define ICMP_RECEIVE_BYTES 2048U
#define IPV4_HEADER_MIN_BYTES 20U
/* IPv4 flags and fragment offset: any set bit other than DF means a fragment. */
#define IPV4_FRAGMENT_MASK 0x3fffU

/* ICMP timestamps count milliseconds since UTC midnight. */
#define ICMP_DAY_MS (24U * 60U * MINUTE / MILLISECOND)

#endif
