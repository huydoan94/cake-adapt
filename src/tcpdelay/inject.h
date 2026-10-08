#ifndef TCPDELAY_INJECT_H_INCLUDED
#define TCPDELAY_INJECT_H_INCLUDED

/*
 * Shared by tcpdelay.bpf.c, injector.c and the host tests: the layouts of the
 * injector's maps and the pure policy that decides what happens to a
 * connection. Kernel types work on both sides.
 *
 * Experimental (tcp_timestamp_inject). A SYN without a TCP timestamp, as
 * Windows sends, gets one, so the server stamps its packets and the TCP
 * filter can measure the connection. A client that accepts the server's
 * timestamp (Windows does) then sends its own, and both directions are
 * measured. Adding that option is the only change made to any packet. When a
 * client or a server rejects it, the server is skipped for a day; the
 * connection that failed is not rescued, only the later ones.
 */
#include <linux/types.h>

#include "common/constants.h"
#include "tcpdelay/record.h"

/* The TSval of an injected SYN; a SYN-ACK that echoes it accepted the timestamp. */
#define TCPDELAY_INJECT_MARKER 0xca4ead01U

/* Each map holds about 100 KB of kernel memory. */
#define TCPDELAY_INJECT_SERVERS 1024
#define TCPDELAY_INJECT_HANDSHAKES 1024
/* A skipped server is tried again after this long. */
#define TCPDELAY_INJECT_SERVER_TTL_NS (24U * 60U * MINUTE * NANOSECONDS_PER_MICROSECOND)

/* The server side of a connection; the address is IPv4-mapped IPv6. */
struct tcpdelay_inject_server_key {
	__u8 address[16];
	/* Network byte order. */
	__u16 port;
	__u16 reserved;
};

/* A server whose SYN or SYN-ACK was rejected; an entry means skip it. */
struct tcpdelay_inject_server {
	/* CLOCK_BOOTTIME nanoseconds of the rejection. */
	__u64 rejected_ns;
	__u64 reserved;
};

enum tcpdelay_inject_handshake_state {
	/* The injected SYN left; no SYN-ACK yet. */
	TCPDELAY_INJECT_SENT = 0,
	/* The server answered. */
	TCPDELAY_INJECT_ANSWERED = 1,
	/* The SYN went unanswered and was resent without a timestamp. */
	TCPDELAY_INJECT_RETRIED = 2,
	/* A rejection was counted; resent SYNs pass unchanged. */
	TCPDELAY_INJECT_CLOSED = 3,
};

/*
 * One injected connection's handshake, keyed by its WAN 4-tuple as a
 * tcpdelay_record_flow (local is the client side after NAT). It is replaced
 * by the next SYN of the same 4-tuple with another sequence number.
 */
struct tcpdelay_inject_handshake {
	__u64 sent_ns;
	/* The SYN's initial sequence number, host byte order. */
	__u32 sequence;
	__u8 state;
	__u8 reserved[3];
};

/* Cumulative since the object was loaded, one copy per CPU. */
struct tcpdelay_inject_counters {
	__u64 injected;
	__u64 skipped;
	__u64 server_accepted;
	__u64 server_declined;
	__u64 client_rejected;
	__u64 server_rejected;
	/* Unanswered injected SYNs resent without a timestamp. */
	__u64 retried;
	/* SYNs the kernel would not let the program resize or rewrite. */
	__u64 failed;
};

/* Written before attaching; the offset of the IP header in a packet. */
struct tcpdelay_inject_settings {
	__u32 network_offset;
	__u32 reserved;
};

_Static_assert(sizeof(struct tcpdelay_inject_server_key) == 20, "server key layout");
_Static_assert(sizeof(struct tcpdelay_inject_server) == 16, "server layout");
_Static_assert(sizeof(struct tcpdelay_inject_handshake) == 16, "handshake layout");
_Static_assert(sizeof(struct tcpdelay_inject_counters) == 64, "counter layout");
_Static_assert(sizeof(struct tcpdelay_inject_settings) == 8, "settings layout");

/* Whether to leave SYNs to this server alone: rejected within the last day. */
static inline int
tcpdelay_inject_server_skipped(const struct tcpdelay_inject_server *server, __u64 now_ns)
{
	return server != 0 && now_ns - server->rejected_ns < TCPDELAY_INJECT_SERVER_TTL_NS;
}

enum tcpdelay_inject_syn {
	/* A new connection: inject unless the server is skipped. */
	TCPDELAY_INJECT_SYN_NEW,
	/* Resent after the SYN-ACK reached the client: the client rejected it. */
	TCPDELAY_INJECT_SYN_REJECTED,
	/* Resent with no SYN-ACK: send it without a timestamp. */
	TCPDELAY_INJECT_SYN_RETRY,
	/* Resent again after a retry or a counted rejection: leave it alone. */
	TCPDELAY_INJECT_SYN_PASS,
};

/* A SYN without a timestamp, against the handshake of its 4-tuple, if any. */
static inline enum tcpdelay_inject_syn
tcpdelay_inject_classify_syn(const struct tcpdelay_inject_handshake *handshake, __u32 sequence)
{
	if (handshake == 0 || handshake->sequence != sequence)
		return TCPDELAY_INJECT_SYN_NEW;
	if (handshake->state == TCPDELAY_INJECT_SENT)
		return TCPDELAY_INJECT_SYN_RETRY;
	if (handshake->state == TCPDELAY_INJECT_ANSWERED)
		return TCPDELAY_INJECT_SYN_REJECTED;
	return TCPDELAY_INJECT_SYN_PASS;
}

/*
 * A client RST with the sequence right after its SYN, once the SYN-ACK
 * arrived: the client refused the handshake before sending anything. An RST
 * after data has a later sequence and is an ordinary close.
 */
static inline int tcpdelay_inject_client_reset_rejects(
	const struct tcpdelay_inject_handshake *handshake,
	__u32 sequence
)
{
	return handshake != 0 && handshake->state == TCPDELAY_INJECT_ANSWERED &&
	       sequence == handshake->sequence + 1U;
}

/* A server RST before any SYN-ACK: the server refused the injected SYN. */
static inline int
tcpdelay_inject_server_reset_rejects(const struct tcpdelay_inject_handshake *handshake)
{
	return handshake != 0 && (handshake->state == TCPDELAY_INJECT_SENT ||
				  handshake->state == TCPDELAY_INJECT_RETRIED);
}

enum tcpdelay_inject_answer {
	/* Not a first answer to an injected SYN. */
	TCPDELAY_INJECT_ANSWER_NONE,
	TCPDELAY_INJECT_ANSWER_ACCEPTED,
	/* Answered without a timestamp: harmless, nothing to remember. */
	TCPDELAY_INJECT_ANSWER_DECLINED,
	/* Answered only once resent without a timestamp: the server drops them. */
	TCPDELAY_INJECT_ANSWER_REJECTED,
};

/* A SYN-ACK, against the handshake; echoes_marker when its TSecr is our TSval. */
static inline enum tcpdelay_inject_answer
tcpdelay_inject_classify_answer(const struct tcpdelay_inject_handshake *handshake, int echoes_marker)
{
	if (handshake == 0)
		return TCPDELAY_INJECT_ANSWER_NONE;
	if (handshake->state == TCPDELAY_INJECT_SENT)
		return echoes_marker ? TCPDELAY_INJECT_ANSWER_ACCEPTED :
				       TCPDELAY_INJECT_ANSWER_DECLINED;
	if (handshake->state == TCPDELAY_INJECT_RETRIED)
		return echoes_marker ? TCPDELAY_INJECT_ANSWER_ACCEPTED :
				       TCPDELAY_INJECT_ANSWER_REJECTED;
	return TCPDELAY_INJECT_ANSWER_NONE;
}

#endif
