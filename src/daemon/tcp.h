#ifndef DAEMON_TCP_H_INCLUDED
#define DAEMON_TCP_H_INCLUDED

/* tcp.c: the TCP queue estimate, the upload ACK rate and timestamp injection. */

#include "controller/controller.h"
#include "platform/netlink.h"
#include "tcpdelay/capture.h"
#include "tcpdelay/estimator.h"
#include "tcpdelay/injector.h"

#include <stdbool.h>
#include <stdint.h>

struct daemon;

struct daemon_tcp {
	struct tcpdelay_capture capture;
	/* Attached while the capture is open, to the same interface. */
	struct tcpdelay_injector injector;
	bool open;
	/* The upload CAKE the capture was opened for; another one needs a new capture. */
	struct netlink_qdisc_id qdisc;
	/* Upload interface whose capture failed; retried once it is recreated. */
	unsigned int failed_index;
	uint64_t dropped_records;
	uint64_t next_counter_check_us;
	/* Pure-ACK and upload byte counters at the last rate sample, and the rates since. */
	bool ack_sampled;
	bool ack_rate_valid;
	bool ack_degraded;
	uint64_t ack_bytes;
	uint64_t upload_bytes;
	uint64_t unaccounted_packets;
	uint64_t ack_sampled_us;
	uint64_t ack_rate_bps;
	uint64_t upload_rate_bps;
};

/* The capture's unloaded state; first, so shutdown is safe from any point. */
void tcp_init(struct daemon *daemon);

/*
 * Loads the TCP filter when a TCP feature is enabled, before pingers start:
 * the kernel's verifier can take seconds on slow CPUs.
 */
void tcp_start(struct daemon *daemon);

void tcp_observe(struct daemon *daemon, struct controller_input *input);

/* Empties the ring buffer between ping replies; see tcp.c. */
void tcp_drain(struct daemon *daemon);

void tcp_close(struct daemon *daemon);

/* Closes and unloads the capture. */
void tcp_stop(struct daemon *daemon);

/* The TCP queue estimate now; false while no capture is open. */
bool tcp_estimate(struct daemon *daemon, uint64_t timestamp_us, struct tcpdelay_estimate *estimate);

#endif
