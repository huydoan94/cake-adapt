#include "tcpdelay/lifetime.h"

#include <string.h>

#define TCPDELAY_DEPARTURE_WAYS 4U
#define TCPDELAY_DEPARTURE_BUCKETS (TCPDELAY_DEPARTURE_SLOTS / TCPDELAY_DEPARTURE_WAYS)
#define FNV_OFFSET_BASIS UINT32_C(2166136261)
#define FNV_PRIME UINT32_C(16777619)

enum tcpdelay_departure_operation {
	TCPDELAY_DEPARTURE_LOOKUP,
	TCPDELAY_DEPARTURE_RECORD,
};

static uint32_t hash_byte(uint32_t hash, uint8_t value)
{
	return (hash ^ value) * FNV_PRIME;
}

static uint32_t hash_flow(const struct tcpdelay_record_flow *flow)
{
	const uint8_t *bytes = (const uint8_t *)flow;
	uint32_t hash = FNV_OFFSET_BASIS;
	size_t index;

	for (index = 0U; index < sizeof(*flow); index++)
		hash = hash_byte(hash, bytes[index]);
	return hash;
}

static uint32_t
hash_departure(const struct tcpdelay_record_flow *flow, uint64_t generation, uint32_t tsval)
{
	uint32_t hash = hash_flow(flow);
	unsigned int index;

	for (index = 0U; index < 8U; index++)
		hash = hash_byte(hash, (uint8_t)(generation >> (index * 8U)));
	for (index = 0U; index < 4U; index++)
		hash = hash_byte(hash, (uint8_t)(tsval >> (index * 8U)));
	return hash;
}

static struct tcpdelay_lifetime_slot *
flow_find(struct tcpdelay_lifetime *lifetime, const struct tcpdelay_record_flow *flow)
{
	uint32_t start = hash_flow(flow) % TCPDELAY_LIFETIME_SLOTS;
	uint32_t probe;
	struct tcpdelay_lifetime_slot *unused = NULL;

	for (probe = 0U; probe < TCPDELAY_LIFETIME_SLOTS; probe++) {
		uint32_t index = (start + probe) % TCPDELAY_LIFETIME_SLOTS;
		struct tcpdelay_lifetime_slot *slot = &lifetime->flows[index];

		if (!slot->used) {
			if (unused == NULL)
				unused = slot;
			continue;
		}
		if (memcmp(&slot->flow, flow, sizeof(*flow)) == 0)
			return slot;
	}
	if (unused == NULL)
		return NULL;
	memset(unused, 0, sizeof(*unused));
	unused->flow = *flow;
	unused->used = true;
	return unused;
}

static struct tcpdelay_lifetime_departure *departure_find(
	struct tcpdelay_lifetime *lifetime,
	const struct tcpdelay_record_flow *flow,
	uint64_t generation,
	uint32_t tsval,
	enum tcpdelay_departure_operation operation,
	uint64_t now_ns
)
{
	uint32_t bucket = hash_departure(flow, generation, tsval) % TCPDELAY_DEPARTURE_BUCKETS;
	struct tcpdelay_lifetime_departure *oldest = NULL;
	uint32_t way;

	for (way = 0U; way < TCPDELAY_DEPARTURE_WAYS; way++) {
		struct tcpdelay_lifetime_departure *entry =
			&lifetime->departures[bucket * TCPDELAY_DEPARTURE_WAYS + way];

		if (entry->used && entry->generation == generation && entry->tsval == tsval &&
		    memcmp(&entry->flow, flow, sizeof(*flow)) == 0)
			return entry;
		if (!entry->used)
			oldest = entry;
		else if (oldest == NULL || entry->departed_ns < oldest->departed_ns)
			oldest = entry;
	}
	if (operation != TCPDELAY_DEPARTURE_RECORD || oldest == NULL)
		return NULL;
	*oldest = (struct tcpdelay_lifetime_departure){
		.flow = *flow,
		.generation = generation,
		.departed_ns = now_ns,
		.tsval = tsval,
		.used = true,
	};
	return oldest;
}

static bool
pair_equal(const struct tcpdelay_lifetime_pair *pair, uint32_t local_isn, uint32_t remote_isn)
{
	return pair->local_isn == local_isn && pair->remote_isn == remote_isn;
}

static bool
pair_seen(const struct tcpdelay_lifetime_slot *slot, uint32_t local_isn, uint32_t remote_isn)
{
	size_t index;

	for (index = 0U; index < slot->history_count; index++)
		if (pair_equal(&slot->history[index], local_isn, remote_isn))
			return true;
	return false;
}

static void close_flow(
	struct tcpdelay_lifetime_slot *slot,
	struct tcpdelay_estimator *estimator,
	uint64_t boundary_ns
)
{
	tcpdelay_estimator_forget(estimator, &slot->flow);
	slot->active = false;
	slot->quarantined = true;
	slot->boundary_ns = boundary_ns;
}

static bool allocate_generation(struct tcpdelay_lifetime *lifetime, uint64_t *generation)
{
	if (lifetime->next_generation == UINT32_MAX)
		return false;
	lifetime->next_generation++;
	*generation = ((uint64_t)lifetime->epoch << 32U) | lifetime->next_generation;
	return true;
}

static bool confirm_pair(
	struct tcpdelay_lifetime *lifetime,
	struct tcpdelay_lifetime_slot *slot,
	struct tcpdelay_estimator *estimator,
	uint64_t boundary_ns
)
{
	uint32_t local_isn = slot->pending_isn[0];
	uint32_t remote_isn = slot->pending_isn[1];
	uint64_t generation;

	if (slot->active && slot->confirmed && slot->local_isn == local_isn &&
	    slot->remote_isn == remote_isn) {
		slot->pending[0] = false;
		slot->pending[1] = false;
		return true;
	}
	if (pair_seen(slot, local_isn, remote_isn)) {
		slot->active = false;
		slot->quarantined = true;
		slot->boundary_ns = boundary_ns;
		slot->pending[0] = false;
		slot->pending[1] = false;
		return true;
	}
	if (slot->history_count >= TCPDELAY_LIFETIME_HISTORY) {
		slot->quarantined = true;
		slot->active = false;
		slot->boundary_ns = boundary_ns;
		return true;
	}
	if (!allocate_generation(lifetime, &generation))
		return false;
	tcpdelay_estimator_forget(estimator, &slot->flow);
	slot->history[slot->history_count++] = (struct tcpdelay_lifetime_pair){
		.local_isn = local_isn,
		.remote_isn = remote_isn,
	};
	slot->local_isn = local_isn;
	slot->remote_isn = remote_isn;
	slot->generation = generation;
	slot->boundary_ns = boundary_ns;
	slot->confirmed = true;
	slot->active = true;
	slot->quarantined = false;
	slot->pending[0] = false;
	slot->pending[1] = false;
	return true;
}

void tcpdelay_lifetime_reset(struct tcpdelay_lifetime *lifetime, uint32_t epoch)
{
	memset(lifetime, 0, sizeof(*lifetime));
	lifetime->epoch = epoch;
}

void tcpdelay_lifetime_init(struct tcpdelay_lifetime *lifetime, uint32_t epoch)
{
	tcpdelay_lifetime_reset(lifetime, epoch);
}

bool tcpdelay_record_valid(const struct tcpdelay_record *record, uint32_t expected_epoch)
{
	uint32_t metadata = record->metadata;
	uint32_t version = (metadata & TCPDELAY_STREAM_VERSION_MASK) >>
			   TCPDELAY_STREAM_VERSION_SHIFT;
	uint32_t epoch = (metadata & TCPDELAY_STREAM_EPOCH_MASK) >> (TCPDELAY_STREAM_EPOCH_SHIFT);
	uint32_t flags = metadata & TCPDELAY_STREAM_FLAG_MASK;
	bool has_timestamp = (metadata & TCPDELAY_STREAM_HAS_TIMESTAMP) != 0U;
	bool lifecycle = (flags & (TCPDELAY_TCP_SYN | TCPDELAY_TCP_FIN | TCPDELAY_TCP_RST)) != 0U;

	return version == TCPDELAY_STREAM_VERSION && epoch == expected_epoch && epoch != 0U &&
	       epoch <= TCPDELAY_STREAM_EPOCHS && (lifecycle || has_timestamp) &&
	       (has_timestamp || (record->tsval == 0U && record->tsecr == 0U));
}

bool tcpdelay_lifetime_add(
	struct tcpdelay_lifetime *lifetime,
	struct tcpdelay_estimator *estimator,
	const struct tcpdelay_record *record,
	uint64_t *departure_ns
)
{
	uint32_t metadata = record->metadata;
	uint32_t flags = metadata & TCPDELAY_STREAM_FLAG_MASK;
	bool outgoing = (metadata & TCPDELAY_STREAM_OUTGOING) != 0U;
	bool has_timestamp = (metadata & TCPDELAY_STREAM_HAS_TIMESTAMP) != 0U;
	bool syn = (flags & TCPDELAY_TCP_SYN) != 0U;
	bool ack = (flags & TCPDELAY_TCP_ACK) != 0U;
	bool closure = (flags & (TCPDELAY_TCP_FIN | TCPDELAY_TCP_RST)) != 0U;
	struct tcpdelay_lifetime_slot *slot;

	*departure_ns = 0U;
	if (!tcpdelay_record_valid(record, lifetime->epoch)) {
		lifetime->uncertain = true;
		return false;
	}
	if (lifetime->uncertain)
		return false;
	slot = flow_find(lifetime, &record->flow);
	if (slot == NULL) {
		lifetime->uncertain = true;
		return false;
	}
	if (record->arrival_ns < slot->boundary_ns)
		return true;
	if (syn && !ack) {
		unsigned int side = outgoing ? 0U : 1U;

		if (slot->active && slot->confirmed &&
		    record->sequence == (side == 0U ? slot->local_isn : slot->remote_isn)) {
			slot->pending[side] = true;
			slot->pending_isn[side] = record->sequence;
			return true;
		}
		if (!slot->pending[side] || slot->pending_isn[side] != record->sequence) {
			if (slot->active)
				close_flow(slot, estimator, record->arrival_ns);
			slot->pending[side] = true;
			slot->pending_isn[side] = record->sequence;
			slot->boundary_ns = record->arrival_ns;
		}
		return true;
	}
	if (syn && ack) {
		unsigned int side = outgoing ? 0U : 1U;
		unsigned int opposite = side == 0U ? 1U : 0U;
		uint32_t opposite_isn;

		if (!slot->pending[opposite])
			return true;
		opposite_isn = slot->pending_isn[opposite];
		if (opposite_isn + 1U != record->acknowledgment)
			return true;
		slot->pending[side] = true;
		slot->pending_isn[side] = record->sequence;
		if (slot->pending[0] && slot->pending[1] &&
		    !confirm_pair(lifetime, slot, estimator, record->arrival_ns)) {
			lifetime->uncertain = true;
			return false;
		}
		return true;
	}
	if (closure) {
		close_flow(slot, estimator, record->arrival_ns);
		slot->pending[0] = false;
		slot->pending[1] = false;
		return true;
	}
	if (!has_timestamp)
		return true;
	if (!slot->active) {
		if (slot->quarantined || slot->history_count != 0U || slot->pending[0] ||
		    slot->pending[1])
			return true;
		if (!allocate_generation(lifetime, &slot->generation)) {
			lifetime->uncertain = true;
			return false;
		}
		slot->active = true;
		slot->boundary_ns = record->arrival_ns;
	}
	if (outgoing) {
		(void)departure_find(
			lifetime,
			&record->flow,
			slot->generation,
			record->tsval,
			TCPDELAY_DEPARTURE_RECORD,
			record->arrival_ns
		);
		return true;
	}
	if (record->tsecr != 0U) {
		struct tcpdelay_lifetime_departure *entry = departure_find(
			lifetime,
			&record->flow,
			slot->generation,
			record->tsecr,
			TCPDELAY_DEPARTURE_LOOKUP,
			0U
		);

		if (entry != NULL)
			*departure_ns = entry->departed_ns;
	}
	{
		struct tcpdelay_sample sample = {
			.flow = record->flow,
			.arrival_ns = record->arrival_ns,
			.departure_ns = *departure_ns,
			.tsval = record->tsval,
			.generation = slot->generation,
		};

		tcpdelay_estimator_add(estimator, &sample);
	}
	return true;
}
