#ifndef CONTROLLER_DIRECTION_H_INCLUDED
#define CONTROLLER_DIRECTION_H_INCLUDED

/*
 * Private to controller/: how controller.c's update of one direction reaches
 * the rate arithmetic (rate.c), the line and congestion state (congestion.c)
 * and the rules that tie download to upload (coupling.c).
 */

#include "controller/controller.h"

#include <stdbool.h>
#include <stdint.h>

/* One direction's share of a controller_update(), after attribution. */
struct direction_update {
	const struct controller_direction_input *input;
	struct controller_latency_input latency;
	/* Bufferbloat is blamed on this direction, so a detected bufferbloat may cut its rate. */
	bool bufferbloat_attributed;
	/* The ACK share ceiling for download; UINT64_MAX when none applies. */
	uint64_t ceiling_bps;
	uint64_t timestamp_us;
};

/* rate.c */

/* Down to a rate CAKE can hold: whole bytes per second. */
uint64_t rate_holdable(uint64_t rate_bps);

enum controller_rate_reason rate_adjust(
	struct controller_direction *direction,
	const struct controller *controller,
	const struct direction_update *update
);

void rate_set_output(
	const struct controller_direction *direction,
	const struct controller_direction_input *input,
	enum controller_rate_reason reason,
	struct controller_direction_output *output
);

/* congestion.c */

enum controller_line_state congestion_update_line_state(
	struct controller_direction *direction,
	const struct controller_direction_input *input
);

enum controller_congestion_state congestion_update(
	struct controller_direction *direction,
	const struct controller_config *config,
	const struct controller_latency_input *latency
);

/* coupling.c */

/*
 * While upload is under high load, ACKs may use what the other traffic leaves
 * free, less a headroom in which that traffic's growth shows, but never less
 * than their minimum share. ACK rate follows download rate, so download is
 * held at the rate whose ACKs fill exactly that allowance.
 */
uint64_t coupling_download_ceiling_bps(
	const struct controller *controller,
	const struct controller_input *input
);

/* A direction holding at least QUEUE_SHARE_RATIO_E6 of a positive total queue. */
bool coupling_holds_queue_share(int64_t queue_us, int64_t total_us);

/* What a direction delivers of its shaper rate; zero without a valid measurement. */
uint64_t coupling_delivery_ratio_e6(
	const struct controller_direction *direction,
	const struct controller_direction_input *input
);

#endif
