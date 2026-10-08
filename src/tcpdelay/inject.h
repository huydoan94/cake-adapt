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
 * RFC 7323), comparing 32-bit values by their signed difference. Windows
 * counts milliseconds since boot, so an injected 1 is behind every clock
 * from 1 ms to 24.8 days of uptime (2^31 ms). An older clock makes the
 * server drop the client's handshake ACK and resend its SYN-ACK; that stalled
 * handshake skips the server, and the daemon pauses injection when stalls
 * repeat. Behind NAT the program cannot tell clients apart, so it cannot
 * learn each one's clock.
 */
#include <linux/types.h>

#include "common/constants.h"
#include "tcpdelay/record.h"

/*
 * The TSval of an injected SYN; a SYN-ACK of the same 4-tuple that echoes it
 * accepted the timestamp. Not zero, which some stacks read as no echo.
 */
#define TCPDELAY_INJECT_TSVAL 1U

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
_Static_assert(sizeof(struct tcpdelay_inject_handshake) == 16, "handshake layout");
_Static_assert(sizeof(struct tcpdelay_inject_counters) == 72, "counter layout");
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

/* Whether the server has answered the injected SYN, with or without the timestamp. */
static inline int tcpdelay_inject_answered(const struct tcpdelay_inject_handshake *handshake)
{
	return handshake->state == TCPDELAY_INJECT_ANSWERED ||
	       handshake->state == TCPDELAY_INJECT_ACCEPTED;
}

/* A SYN without a timestamp, against the handshake of its 4-tuple, if any. */
static inline enum tcpdelay_inject_syn
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
static inline int tcpdelay_inject_client_reset_rejects(
	const struct tcpdelay_inject_handshake *handshake,
	__u32 sequence
)
{
	return handshake != 0 && tcpdelay_inject_answered(handshake) &&
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
	/* Answered again after accepting: the client's handshake ACK was dropped. */
	TCPDELAY_INJECT_ANSWER_STALLED,
};

/* A SYN-ACK, against the handshake; echoes when its TSecr is the injected TSval. */
static inline enum tcpdelay_inject_answer
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
 * Userspace: pauses injection when handshakes stall repeatedly. Behind NAT a
 * client whose clock looks older than the injected TSval cannot be told apart,
 * and every new server would cost it a hung connection, so injection stops
 * for everyone for a while. Times are microseconds.
 */
#define TCPDELAY_INJECT_STALL_LIMIT 3U
#define TCPDELAY_INJECT_STALL_WINDOW_US (10U * MINUTE)
#define TCPDELAY_INJECT_PAUSE_US (24U * 60U * MINUTE)
/* Checks remembered; at one a minute, more than the window holds. */
#define TCPDELAY_INJECT_BREAKER_SAMPLES 16U

struct tcpdelay_inject_breaker {
	/* The stalled counter at recent checks, oldest overwritten first. */
	__u64 sample_us[TCPDELAY_INJECT_BREAKER_SAMPLES];
	__u64 sample_stalled[TCPDELAY_INJECT_BREAKER_SAMPLES];
	__u32 samples;
	__u32 next;
	int paused;
	__u64 paused_until_us;
};

enum tcpdelay_inject_breaker_action {
	TCPDELAY_INJECT_BREAKER_KEEP,
	/* Stalls repeated: detach until paused_until_us. */
	TCPDELAY_INJECT_BREAKER_PAUSE,
	/* The pause is over: attach again. */
	TCPDELAY_INJECT_BREAKER_RESUME,
};

/*
 * One check of the stalled counter, cumulative since the object was loaded
 * (from zero), at now_us. It pauses when the counter rose by
 * TCPDELAY_INJECT_STALL_LIMIT or more since the oldest remembered check
 * within the window, or since zero before any check, and resumes after the
 * pause with a history starting at that check.
 */
static inline enum tcpdelay_inject_breaker_action
tcpdelay_inject_breaker_check(struct tcpdelay_inject_breaker *breaker, __u64 stalled, __u64 now_us)
{
	enum tcpdelay_inject_breaker_action action = TCPDELAY_INJECT_BREAKER_KEEP;
	__u64 baseline = 0U;
	__u32 i;

	if (breaker->paused) {
		if (now_us < breaker->paused_until_us)
			return TCPDELAY_INJECT_BREAKER_KEEP;
		breaker->paused = 0;
		breaker->samples = 0U;
		breaker->next = 0U;
		baseline = stalled;
		action = TCPDELAY_INJECT_BREAKER_RESUME;
	}
	/* The counter only grows, so the smallest value in the window is its start. */
	if (breaker->samples != 0U)
		baseline = stalled;
	for (i = 0U; i < breaker->samples; i++)
		if (now_us - breaker->sample_us[i] <= TCPDELAY_INJECT_STALL_WINDOW_US &&
		    breaker->sample_stalled[i] < baseline)
			baseline = breaker->sample_stalled[i];
	breaker->sample_us[breaker->next] = now_us;
	breaker->sample_stalled[breaker->next] = stalled;
	breaker->next = (breaker->next + 1U) % TCPDELAY_INJECT_BREAKER_SAMPLES;
	if (breaker->samples < TCPDELAY_INJECT_BREAKER_SAMPLES)
		breaker->samples++;
	if (stalled - baseline < TCPDELAY_INJECT_STALL_LIMIT)
		return action;
	breaker->paused = 1;
	breaker->paused_until_us = now_us + TCPDELAY_INJECT_PAUSE_US;
	return TCPDELAY_INJECT_BREAKER_PAUSE;
}

#endif
