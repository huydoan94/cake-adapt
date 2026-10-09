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
 *
 * The client's own timestamps must not look older than the injected TSval:
 * servers drop segments whose TSval is behind the last one they saw (PAWS,
 * RFC 7323), comparing 32-bit values by their signed difference, so one
 * injected value covers the clocks within 2^31 ticks (24.8 days of Windows
 * uptime) after it. An older clock makes the server drop the client's
 * handshake ACK and resend its SYN-ACK: the handshake stalls.
 *
 * IPv6 clients keep their own address, so each client's clock is learned
 * from its first timestamped packets and injected back to it; a stall skips
 * that client and server for a day. Behind IPv4 NAT clients cannot be told
 * apart: all get TSVAL until 5 stalls within 10 seconds; then, if the
 * youngest clock learned within a day is at least a day old, they get that
 * clock instead; 5 more stalls within 10 seconds, or no such clock, pause
 * IPv4 injection for a day, after which it starts over.
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

/* Each map holds about 100 KB of kernel memory. */
#define TCPDELAY_INJECT_SERVERS 1024
#define TCPDELAY_INJECT_HANDSHAKES 1024
#define TCPDELAY_INJECT_CLIENTS 1024
/* A skip, a pause and a learned clock each last a day. */
#define TCPDELAY_INJECT_DAY_NS (24U * 60U * MINUTE * NANOSECONDS_PER_MICROSECOND)
/* IPv4 injection changes after this many stalls within the configured window. */
#define TCPDELAY_INJECT_STALL_BURST 5U
/*
 * Client clocks are compared in milliseconds since our boot, the tick of
 * Windows timestamps. The youngest IPv4 clock replaces TSVAL only when it is
 * at least a day old, less a margin for clocks read a little early.
 */
#define TCPDELAY_INJECT_TICK_NS NANOSECONDS_PER_MILLISECOND
#define TCPDELAY_INJECT_SWITCH_MIN_TICKS ((__u32)(24U * 60U * MINUTE / MILLISECOND))
#define TCPDELAY_INJECT_SWITCH_MARGIN_TICKS ((__u32)(10U * MINUTE / MILLISECOND))
/* IPv4 clocks the filter remembers, oldest overwritten first; a power of two. */
#define TCPDELAY_INJECT_CLOCKS 32U

/*
 * A skipped server, with the client for IPv6 (zero for IPv4, where NAT hides
 * it); addresses are IPv4-mapped IPv6.
 */
struct tcpdelay_inject_server_key {
	__u8 client[16];
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
	/* The server answered without the timestamp. */
	TCPDELAY_INJECT_ANSWERED = 1,
	/* The SYN went unanswered and was resent without a timestamp. */
	TCPDELAY_INJECT_RETRIED = 2,
	/* A rejection was counted; resent SYNs pass unchanged. */
	TCPDELAY_INJECT_CLOSED = 3,
	/* The server answered and echoed the timestamp. */
	TCPDELAY_INJECT_ACCEPTED = 4,
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
	/* The injected TSval, which an accepting SYN-ACK echoes. */
	__u32 tsval;
	__u8 state;
	/* The client's clock was learned from this connection. */
	__u8 learned;
	/* IPv6, where the client keeps its own address; set from the SYN. */
	__u8 ipv6;
	__u8 reserved[5];
};

/* An IPv6 client, by its address. */
struct tcpdelay_inject_client_key {
	__u8 address[16];
};

/* An IPv6 client's latest timestamp, learned from a connection we injected. */
struct tcpdelay_inject_client {
	__u64 seen_ns;
	__u32 tsval;
	__u32 reserved;
};

enum tcpdelay_inject_mode {
	/* Injecting TSVAL. */
	TCPDELAY_INJECT_NORMAL = 0,
	/* Injecting the youngest learned clock after a stall burst. */
	TCPDELAY_INJECT_SWITCHED = 1,
	/* Not injecting IPv4 until paused_until_ns. */
	TCPDELAY_INJECT_PAUSED = 2,
};

/* A learned IPv4 clock: its TSval and when it was seen (CLOCK_BOOTTIME). */
struct tcpdelay_inject_clock {
	__u64 seen_ns;
	__u32 tsval;
	__u32 reserved;
};

/* IPv4 injection, shared by every CPU; racy updates only shift a stall or a clock. */
struct tcpdelay_inject_state {
	/* The latest stalls, oldest overwritten first. */
	__u64 stall_ns[TCPDELAY_INJECT_STALL_BURST];
	__u64 paused_until_ns;
	/*
	 * The latest IPv4 clocks learned, raw: the filter only stores them (the
	 * 32-bit x86 JIT has no 64-bit division), and the daemon reads them.
	 */
	struct tcpdelay_inject_clock clocks[TCPDELAY_INJECT_CLOCKS];
	__u32 next_clock;
	__u32 next_stall;
	/* The IPv4 TSval while NORMAL or SWITCHED. */
	__u32 tsval;
	__u32 mode;
	/*
	 * A stall burst the filter saw, for the injector to act on at the next
	 * IPv4 SYN; the filter stays small enough for the verifier.
	 */
	__u32 burst;
	/* Explicit: 32-bit x86 aligns __u64 to 4 bytes, BPF to 8. */
	__u32 reserved;
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
	/*
	 * Accepted handshakes whose SYN-ACK came again: the server never took
	 * the client's ACK, as when the client's clock looks older than ours.
	 */
	__u64 stalled_ipv4;
	__u64 stalled_ipv6;
	/* IPv4 SYNs left alone while paused. */
	__u64 paused;
};

/* Written before attaching. */
struct tcpdelay_inject_settings {
	/* TCPDELAY_INJECT_STALL_BURST IPv4 stalls within this make a burst. */
	__u64 stall_window_ns;
	/* The offset of the IP header in a packet. */
	__u32 network_offset;
	__u32 reserved;
};

_Static_assert(sizeof(struct tcpdelay_inject_server_key) == 36, "server key layout");
_Static_assert(sizeof(struct tcpdelay_inject_server) == 16, "server layout");
_Static_assert(sizeof(struct tcpdelay_inject_handshake) == 24, "handshake layout");
_Static_assert(sizeof(struct tcpdelay_inject_client_key) == 16, "client key layout");
_Static_assert(sizeof(struct tcpdelay_inject_client) == 16, "client layout");
_Static_assert(sizeof(struct tcpdelay_inject_state) == 584, "state layout");
_Static_assert(sizeof(struct tcpdelay_inject_counters) == 88, "counter layout");
_Static_assert(sizeof(struct tcpdelay_inject_settings) == 16, "settings layout");

/* Whether to leave SYNs to this server alone: rejected within the last day. */
INJECT_INLINE int
tcpdelay_inject_server_skipped(const struct tcpdelay_inject_server *server, __u64 now_ns)
{
	return server != 0 && now_ns - server->rejected_ns < TCPDELAY_INJECT_DAY_NS;
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

/* Whether the server has answered the injected SYN, with or without the timestamp. */
INJECT_INLINE int tcpdelay_inject_answered(const struct tcpdelay_inject_handshake *handshake)
{
	return handshake->state == TCPDELAY_INJECT_ANSWERED ||
	       handshake->state == TCPDELAY_INJECT_ACCEPTED;
}

/* A SYN without a timestamp, against the handshake of its 4-tuple, if any. */
INJECT_INLINE enum tcpdelay_inject_syn
tcpdelay_inject_classify_syn(const struct tcpdelay_inject_handshake *handshake, __u32 sequence)
{
	if (handshake == 0 || handshake->sequence != sequence)
		return TCPDELAY_INJECT_SYN_NEW;
	if (handshake->state == TCPDELAY_INJECT_SENT)
		return TCPDELAY_INJECT_SYN_RETRY;
	if (tcpdelay_inject_answered(handshake))
		return TCPDELAY_INJECT_SYN_REJECTED;
	return TCPDELAY_INJECT_SYN_PASS;
}

/*
 * A client RST with the sequence right after its SYN, once the SYN-ACK
 * arrived: the client refused the handshake before sending anything. An RST
 * after data has a later sequence and is an ordinary close.
 */
INJECT_INLINE int tcpdelay_inject_client_reset_rejects(
	const struct tcpdelay_inject_handshake *handshake,
	__u32 sequence
)
{
	return handshake != 0 && tcpdelay_inject_answered(handshake) &&
	       sequence == handshake->sequence + 1U;
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
	TCPDELAY_INJECT_ANSWER_ACCEPTED,
	/* Answered without a timestamp: harmless, nothing to remember. */
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
		return echoes ? TCPDELAY_INJECT_ANSWER_ACCEPTED : TCPDELAY_INJECT_ANSWER_DECLINED;
	if (handshake->state == TCPDELAY_INJECT_RETRIED)
		return echoes ? TCPDELAY_INJECT_ANSWER_ACCEPTED : TCPDELAY_INJECT_ANSWER_REJECTED;
	/*
	 * Without timestamps nothing can be dropped as old, so only a handshake
	 * that took ours stalls; a lost ACK looks the same and costs a day.
	 */
	if (handshake->state == TCPDELAY_INJECT_ACCEPTED)
		return TCPDELAY_INJECT_ANSWER_STALLED;
	return TCPDELAY_INJECT_ANSWER_NONE;
}

/*
 * The IPv4 TSval for a new SYN, or 0 while paused or while a stall burst the
 * filter recorded waits for the daemon (tcpdelay_inject_resolve()). Read-only,
 * so the injector stays small enough for the verifier.
 */
INJECT_INLINE __u32 tcpdelay_inject_ipv4_tsval(const struct tcpdelay_inject_state *state)
{
	if (state->burst || state->mode == TCPDELAY_INJECT_PAUSED)
		return 0U;
	if (state->mode == TCPDELAY_INJECT_SWITCHED && state->tsval != 0U)
		return state->tsval;
	return TCPDELAY_INJECT_TSVAL;
}

/* Records an IPv4 stall; whether it completes a burst within window_ns. */
INJECT_INLINE int
tcpdelay_inject_stall_burst(struct tcpdelay_inject_state *state, __u64 now_ns, __u64 window_ns)
{
	/*
	 * Range checks, not a modulo: the verifier cannot bound x % 5, which
	 * compiles to a multiplication and a shift.
	 */
	__u32 slot = state->next_stall;
	__u32 next;

	if (slot >= TCPDELAY_INJECT_STALL_BURST)
		slot = 0U;
	next = slot + 1U;
	if (next >= TCPDELAY_INJECT_STALL_BURST)
		next = 0U;
	state->stall_ns[slot] = now_ns;
	state->next_stall = next;
	/* After this one, the next slot holds the oldest of the last BURST stalls. */
	return state->stall_ns[next] != 0U && now_ns - state->stall_ns[next] <= window_ns;
}

/* Remembers an IPv4 client's clock; the filter's part, without division. */
INJECT_INLINE void
tcpdelay_inject_learn_clock(struct tcpdelay_inject_state *state, __u32 tsval, __u64 now_ns)
{
	/* CLOCKS is a power of two, so this is a mask the verifier can bound. */
	__u32 slot = state->next_clock % TCPDELAY_INJECT_CLOCKS;

	state->clocks[slot].seen_ns = now_ns;
	state->clocks[slot].tsval = tsval;
	state->next_clock = slot + 1U;
}

/*
 * Userspace: the youngest IPv4 clock seen within a day, as it reads now (its
 * TSval plus the milliseconds since); 0 when none. A younger Windows clock
 * reads less, and unsigned readings order clocks any distance apart.
 */
INJECT_INLINE __u32
tcpdelay_inject_youngest_clock(const struct tcpdelay_inject_state *state, __u64 now_ns)
{
	__u32 youngest = 0U;
	__u32 i;

	for (i = 0U; i < TCPDELAY_INJECT_CLOCKS; i++) {
		const struct tcpdelay_inject_clock *clock = &state->clocks[i];
		__u32 reading;

		if (clock->seen_ns == 0U || now_ns - clock->seen_ns >= TCPDELAY_INJECT_DAY_NS)
			continue;
		reading =
			clock->tsval + (__u32)((now_ns - clock->seen_ns) / TCPDELAY_INJECT_TICK_NS);
		if (youngest == 0U || reading < youngest)
			youngest = reading;
	}
	return youngest;
}

/*
 * After a stall burst: switch to the youngest learned clock when NORMAL and it
 * is at least a day old, else pause for a day. Whether IPv4 is now paused.
 */
INJECT_INLINE int tcpdelay_inject_escalate(struct tcpdelay_inject_state *state, __u64 now_ns)
{
	__u32 youngest = tcpdelay_inject_youngest_clock(state, now_ns);

	__builtin_memset(state->stall_ns, 0, sizeof(state->stall_ns));
	if (state->mode == TCPDELAY_INJECT_NORMAL && youngest >= TCPDELAY_INJECT_SWITCH_MIN_TICKS) {
		state->mode = TCPDELAY_INJECT_SWITCHED;
		state->tsval = youngest - TCPDELAY_INJECT_SWITCH_MARGIN_TICKS;
		return 0;
	}
	state->mode = TCPDELAY_INJECT_PAUSED;
	state->paused_until_ns = now_ns + TCPDELAY_INJECT_DAY_NS;
	return 1;
}

/* An IPv6 client's learned TSval when fresh (a day), else TSVAL. */
INJECT_INLINE __u32
tcpdelay_inject_client_tsval(const struct tcpdelay_inject_client *client, __u64 now_ns)
{
	if (client == 0 || client->tsval == 0U ||
	    now_ns - client->seen_ns >= TCPDELAY_INJECT_DAY_NS)
		return TCPDELAY_INJECT_TSVAL;
	return client->tsval;
}

enum tcpdelay_inject_change {
	TCPDELAY_INJECT_UNCHANGED,
	TCPDELAY_INJECT_TO_SWITCHED,
	TCPDELAY_INJECT_TO_PAUSED,
	TCPDELAY_INJECT_TO_NORMAL,
};

/*
 * Userspace, every second: acts on a recorded stall burst (switch or pause, as
 * tcpdelay_inject_escalate()) and ends a pause after a day, starting over with
 * TSVAL. The caller writes the state back.
 */
INJECT_INLINE enum tcpdelay_inject_change
tcpdelay_inject_resolve(struct tcpdelay_inject_state *state, __u64 now_ns)
{
	if (state->burst) {
		state->burst = 0U;
		return tcpdelay_inject_escalate(state, now_ns) ? TCPDELAY_INJECT_TO_PAUSED :
								 TCPDELAY_INJECT_TO_SWITCHED;
	}
	if (state->mode == TCPDELAY_INJECT_PAUSED && now_ns >= state->paused_until_ns) {
		__builtin_memset(state->stall_ns, 0, sizeof(state->stall_ns));
		state->mode = TCPDELAY_INJECT_NORMAL;
		state->tsval = TCPDELAY_INJECT_TSVAL;
		return TCPDELAY_INJECT_TO_NORMAL;
	}
	return TCPDELAY_INJECT_UNCHANGED;
}

#endif
