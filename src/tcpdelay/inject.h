#ifndef TCPDELAY_INJECT_H_INCLUDED
#define TCPDELAY_INJECT_H_INCLUDED

/*
 * Shared by tcpdelay.bpf.c, injector.c and the host tests: the layouts of the
 * injector's maps and the pure policy that decides what happens to a
 * connection. Kernel types work on both sides.
 *
 * Experimental (tcp_ts_request). A SYN without a TCP timestamp, as
 * Windows sends, gets one, so the server stamps its packets and the TCP
 * filter can measure the connection. A client that accepts the server's
 * timestamp (Windows does) then sends its own, and both directions are
 * measured. Adding that option is the only change made to any packet. When a
 * server rejects it, the server is skipped for a day; the connection that
 * failed is not rescued, only the later ones. A client refusing the server's
 * answer counts as the server rejecting it: whatever refused it on the way,
 * the router sees the same failed handshake.
 *
 * The client's own timestamps must not look older than the injected TSval:
 * servers drop segments whose TSval is behind the last one they saw (PAWS,
 * RFC 7323), comparing 32-bit values by their signed difference, so one
 * injected value covers the clocks within 2^31 ticks (24.8 days of Windows
 * uptime) after it. An older clock makes the server drop the client's
 * handshake ACK and resend its SYN-ACK: the handshake stalls.
 *
 * Only IPv6 SYNs are injected: IPv6 clients keep their own address, so each
 * client gets TSVAL, then STALLED_TSVAL if a handshake with it stalls, and its
 * own clock once learned from its first timestamped packets. A stall skips
 * nothing; a server's rejection skips it for every client for a day. Behind
 * IPv4 NAT clients cannot be told apart, and one value for all of them stalls
 * the connections of every client whose clock it does not suit.
 */
#include <linux/types.h>

#include "common/constants.h"
#include "tcpdelay/record.h"

/*
 * Every function here is inlined: a BPF program with calls between functions
 * cannot be JIT-compiled on 32-bit x86, and would run interpreted.
 */
#define INJECT_INLINE static inline __attribute__((always_inline))

/*
 * The TSval of an injected SYN while no better one is known; a SYN-ACK of the
 * same 4-tuple that echoes the injected value accepted the timestamp. Not zero,
 * which some stacks read as no echo.
 */
#define TCPDELAY_INJECT_TSVAL 1U
/*
 * For a client whose handshake stalled with TSVAL, until its clock is
 * learned: half the clock away, so it suits the clocks TSVAL does not (Windows
 * up 24.8 to 49.7 days).
 */
#define TCPDELAY_INJECT_STALLED_TSVAL 0x80000000U

/* Each map holds about 100 KB of kernel memory. */
#define TCPDELAY_INJECT_SERVERS 1024
#define TCPDELAY_INJECT_HANDSHAKES 1024
#define TCPDELAY_INJECT_CLIENTS 1024
/* A skip and a learned clock each last a day. */
#define TCPDELAY_INJECT_DAY_NS (24U * 60U * MINUTE * NANOSECONDS_PER_MICROSECOND)
/* A skipped server, for every client. */
struct tcpdelay_inject_server_key {
	__u8 address[16];
	/* Network byte order. */
	__u16 port;
	__u16 reserved;
};

/* A server that rejected a timestamped SYN; an entry means skip it. */
struct tcpdelay_inject_server {
	/* CLOCK_BOOTTIME nanoseconds of the rejection. */
	__u64 rejected_ns;
	__u64 reserved;
};

enum tcpdelay_inject_handshake_state {
	/* The injected SYN left; no SYN-ACK yet. */
	TCPDELAY_INJECT_SENT = 0,
	/* The SYN went unanswered and was resent without a timestamp. */
	TCPDELAY_INJECT_RETRIED = 2,
	/* A refusal or a stall was counted; resent SYNs pass unchanged. */
	TCPDELAY_INJECT_CLOSED = 3,
	/* The server's SYN-ACK echoed the timestamp; not yet known to work. */
	TCPDELAY_INJECT_ECHOED = 4,
};

/*
 * One injected connection's handshake, keyed by its WAN 4-tuple as a
 * tcpdelay_record_flow (local is the client side after NAT). It is replaced
 * by the next SYN of the same 4-tuple with another sequence number.
 */
struct tcpdelay_inject_handshake {
	/* The SYN's initial sequence number, host byte order. */
	__u32 sequence;
	/* The injected TSval, which an accepting SYN-ACK echoes. */
	__u32 tsval;
	__u8 state;
	/* The client's clock was learned from this connection. */
	__u8 learned;
	__u8 reserved[2];
};

/* A client, by its address. */
struct tcpdelay_inject_client_key {
	__u8 address[16];
};

/*
 * A client's latest timestamp, learned from a connection we injected, or a
 * stall before one was learned.
 */
struct tcpdelay_inject_client {
	/* CLOCK_BOOTTIME nanoseconds of the learned TSval, or of the stall. */
	__u64 seen_ns;
	/* Zero until learned. */
	__u32 tsval;
	/* A handshake of this client stalled. */
	__u32 stalled;
};

/* Cumulative since the object was loaded, one copy per CPU. */
struct tcpdelay_inject_counters {
	__u64 injected;
	/* Refused timestamped handshakes, and SYNs left alone as their server is skipped. */
	__u64 skipped;
	/*
	 * Injected handshakes that worked: the server acknowledged the client's
	 * first data, which a stalled handshake never gets.
	 */
	__u64 accepted;
	/*
	 * Accepted handshakes whose SYN-ACK came again: the server never took
	 * the client's ACK, as when the client's clock looks older than ours.
	 */
	__u64 stalled;
};

/* Written before attaching; the offset of the IP header in a packet. */
struct tcpdelay_inject_settings {
	__u32 network_offset;
	__u32 reserved;
};

_Static_assert(sizeof(struct tcpdelay_inject_server_key) == 20, "server key layout");
_Static_assert(sizeof(struct tcpdelay_inject_server) == 16, "server layout");
_Static_assert(sizeof(struct tcpdelay_inject_handshake) == 12, "handshake layout");
_Static_assert(sizeof(struct tcpdelay_inject_client_key) == 16, "client key layout");
_Static_assert(sizeof(struct tcpdelay_inject_client) == 16, "client layout");
_Static_assert(sizeof(struct tcpdelay_inject_counters) == 32, "counter layout");
_Static_assert(sizeof(struct tcpdelay_inject_settings) == 8, "settings layout");

/* Whether to leave SYNs to this server alone: rejected within the last day. */
INJECT_INLINE int
tcpdelay_inject_server_skipped(const struct tcpdelay_inject_server *server, __u64 now_ns)
{
	return server != 0 && now_ns - server->rejected_ns < TCPDELAY_INJECT_DAY_NS;
}

enum tcpdelay_inject_syn {
	/* A new connection: inject unless the server is skipped. */
	TCPDELAY_INJECT_SYN_NEW,
	/* Resent after the SYN-ACK: the client never took it, a rejection. */
	TCPDELAY_INJECT_SYN_REJECTED,
	/* Resent with no SYN-ACK: send it without a timestamp. */
	TCPDELAY_INJECT_SYN_RETRY,
	/* Resent again after a retry or a counted rejection: leave it alone. */
	TCPDELAY_INJECT_SYN_PASS,
};

/* A SYN without a timestamp, against the handshake of its 4-tuple, if any. */
INJECT_INLINE enum tcpdelay_inject_syn
tcpdelay_inject_classify_syn(const struct tcpdelay_inject_handshake *handshake, __u32 sequence)
{
	if (handshake == 0 || handshake->sequence != sequence)
		return TCPDELAY_INJECT_SYN_NEW;
	if (handshake->state == TCPDELAY_INJECT_SENT)
		return TCPDELAY_INJECT_SYN_RETRY;
	if (handshake->state == TCPDELAY_INJECT_ECHOED)
		return TCPDELAY_INJECT_SYN_REJECTED;
	return TCPDELAY_INJECT_SYN_PASS;
}

/*
 * A client RST with the sequence right after its SYN, once the SYN-ACK
 * arrived: the handshake failed before anything was sent, a rejection like a
 * server's. An RST after data has a later sequence and is an ordinary close.
 */
INJECT_INLINE int tcpdelay_inject_client_reset_rejects(
	const struct tcpdelay_inject_handshake *handshake,
	__u32 sequence
)
{
	return handshake != 0 && handshake->state == TCPDELAY_INJECT_ECHOED &&
	       sequence == handshake->sequence + 1U;
}

/*
 * Whether an acknowledgement number covers data after the SYN whose sequence
 * is sequence: the server took the client's first data, with its timestamp.
 */
INJECT_INLINE int tcpdelay_inject_data_acknowledged(__u32 acknowledgement, __u32 sequence)
{
	return (__s32)(acknowledgement - (sequence + 1U)) > 0;
}

/* A server RST before any SYN-ACK: the server refused the injected SYN. */
INJECT_INLINE int
tcpdelay_inject_server_reset_rejects(const struct tcpdelay_inject_handshake *handshake)
{
	return handshake != 0 && (handshake->state == TCPDELAY_INJECT_SENT ||
				  handshake->state == TCPDELAY_INJECT_RETRIED);
}

enum tcpdelay_inject_answer {
	/* Not a first answer to an injected SYN. */
	TCPDELAY_INJECT_ANSWER_NONE,
	TCPDELAY_INJECT_ANSWER_ECHOED,
	/* Answered without a timestamp: the server does not take them. */
	TCPDELAY_INJECT_ANSWER_DECLINED,
	/* Answered only once resent without a timestamp: the server drops them. */
	TCPDELAY_INJECT_ANSWER_REJECTED,
	/* Answered again after accepting: the client's handshake ACK was dropped. */
	TCPDELAY_INJECT_ANSWER_STALLED,
};

/* A SYN-ACK, against the handshake; echoes when its TSecr is the injected TSval. */
INJECT_INLINE enum tcpdelay_inject_answer
tcpdelay_inject_classify_answer(const struct tcpdelay_inject_handshake *handshake, int echoes)
{
	if (handshake == 0)
		return TCPDELAY_INJECT_ANSWER_NONE;
	if (handshake->state == TCPDELAY_INJECT_SENT)
		return echoes ? TCPDELAY_INJECT_ANSWER_ECHOED : TCPDELAY_INJECT_ANSWER_DECLINED;
	if (handshake->state == TCPDELAY_INJECT_RETRIED)
		return echoes ? TCPDELAY_INJECT_ANSWER_ECHOED : TCPDELAY_INJECT_ANSWER_REJECTED;
	/*
	 * Without timestamps nothing can be dropped as old, so only a handshake
	 * that took ours stalls; a lost ACK looks the same and costs a day.
	 */
	if (handshake->state == TCPDELAY_INJECT_ECHOED)
		return TCPDELAY_INJECT_ANSWER_STALLED;
	return TCPDELAY_INJECT_ANSWER_NONE;
}

/*
 * The TSval for a client's SYN: its learned clock, else STALLED_TSVAL after a
 * stall, else TSVAL; what is known lasts a day.
 */
INJECT_INLINE __u32
tcpdelay_inject_client_tsval(const struct tcpdelay_inject_client *client, __u64 now_ns)
{
	if (client == 0 || now_ns - client->seen_ns >= TCPDELAY_INJECT_DAY_NS)
		return TCPDELAY_INJECT_TSVAL;
	if (client->tsval != 0U)
		return client->tsval;
	return client->stalled ? TCPDELAY_INJECT_STALLED_TSVAL : TCPDELAY_INJECT_TSVAL;
}

#endif
