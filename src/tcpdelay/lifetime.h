#ifndef TCPDELAY_LIFETIME_H_INCLUDED
#define TCPDELAY_LIFETIME_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tcpdelay/estimator.h"

#define TCPDELAY_LIFETIME_CACHE_BYTES (1024U * 1024U)

struct tcpdelay_lifetime_pair {
	uint32_t local_isn;
	uint32_t remote_isn;
};

struct tcpdelay_lifetime_slot {
	struct tcpdelay_record_flow flow;
	struct tcpdelay_lifetime_pair history[TCPDELAY_LIFETIME_HISTORY];
	uint64_t generation;
	uint64_t boundary_ns;
	uint32_t pending_isn[2];
	uint32_t local_isn;
	uint32_t remote_isn;
	uint8_t history_count;
	bool used;
	bool pending[2];
	bool active;
	bool confirmed;
	bool quarantined;
};

struct tcpdelay_lifetime_departure {
	struct tcpdelay_record_flow flow;
	uint64_t generation;
	uint64_t departed_ns;
	uint32_t tsval;
	bool used;
};

struct tcpdelay_lifetime {
	struct tcpdelay_lifetime_slot flows[TCPDELAY_LIFETIME_SLOTS];
	struct tcpdelay_lifetime_departure departures[TCPDELAY_DEPARTURE_SLOTS];
	uint64_t next_generation;
	uint32_t epoch;
	bool uncertain;
};

_Static_assert(
	sizeof(struct tcpdelay_lifetime) <= TCPDELAY_LIFETIME_CACHE_BYTES,
	"TCP lifetime cache must stay below one MiB"
);

void tcpdelay_lifetime_init(struct tcpdelay_lifetime *lifetime, uint32_t epoch);
void tcpdelay_lifetime_reset(struct tcpdelay_lifetime *lifetime, uint32_t epoch);

bool tcpdelay_record_valid(const struct tcpdelay_record *record, uint32_t epoch);

/* False means the ordered event stream or bounded lifetime history is uncertain. */
bool tcpdelay_lifetime_add(
	struct tcpdelay_lifetime *lifetime,
	struct tcpdelay_estimator *estimator,
	const struct tcpdelay_record *record,
	uint64_t *departure_ns
);

#endif
