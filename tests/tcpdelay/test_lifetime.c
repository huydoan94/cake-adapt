#include "tcpdelay/lifetime.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TEST_EPOCH 1U

enum test_packet_direction {
	TEST_PACKET_OUTGOING,
	TEST_PACKET_INCOMING,
};

enum test_timestamp_state {
	TEST_TIMESTAMP_PRESENT,
	TEST_TIMESTAMP_ABSENT,
};

static uint32_t
metadata(uint32_t flags, enum test_packet_direction direction, enum test_timestamp_state timestamp)
{
	return flags | (direction == TEST_PACKET_OUTGOING ? TCPDELAY_STREAM_OUTGOING : 0U) |
	       (timestamp == TEST_TIMESTAMP_PRESENT ? TCPDELAY_STREAM_HAS_TIMESTAMP : 0U) |
	       (TCPDELAY_STREAM_VERSION << TCPDELAY_STREAM_VERSION_SHIFT) |
	       (TEST_EPOCH << TCPDELAY_STREAM_EPOCH_SHIFT);
}

static struct tcpdelay_record record_for(
	uint64_t arrival,
	uint32_t flags,
	enum test_packet_direction direction,
	enum test_timestamp_state timestamp,
	uint32_t sequence,
	uint32_t acknowledgment,
	uint32_t tsval,
	uint32_t tsecr
)
{
	struct tcpdelay_record record = {
		.arrival_ns = arrival,
		.tsval = tsval,
		.tsecr = tsecr,
		.sequence = sequence,
		.acknowledgment = acknowledgment,
		.metadata = metadata(flags, direction, timestamp),
	};

	record.flow.local_address[15] = 2U;
	record.flow.remote_address[15] = 1U;
	record.flow.local_port = 50000U;
	record.flow.remote_port = 443U;
	return record;
}

static bool
add(struct tcpdelay_lifetime *lifetime,
    struct tcpdelay_estimator *estimator,
    struct tcpdelay_record record,
    uint64_t *departure)
{
	return tcpdelay_lifetime_add(lifetime, estimator, &record, departure);
}

static struct tcpdelay_lifetime_slot *find_flow(struct tcpdelay_lifetime *lifetime)
{
	size_t index;

	for (index = 0U; index < TCPDELAY_LIFETIME_SLOTS; index++)
		if (lifetime->flows[index].used)
			return &lifetime->flows[index];
	return NULL;
}

static void establish(
	struct tcpdelay_lifetime *lifetime,
	struct tcpdelay_estimator *estimator,
	uint64_t start,
	uint32_t local_isn,
	uint32_t remote_isn
)
{
	uint64_t departure;

	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    start,
			    TCPDELAY_TCP_SYN,
			    TEST_PACKET_OUTGOING,
			    TEST_TIMESTAMP_ABSENT,
			    local_isn,
			    0U,
			    0U,
			    0U
		    ),
		    &departure)
	);
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    start + 1U,
			    TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK,
			    TEST_PACKET_INCOMING,
			    TEST_TIMESTAMP_PRESENT,
			    remote_isn,
			    local_isn + 1U,
			    900U,
			    0U
		    ),
		    &departure)
	);
}

static void test_reuse_resets_and_old_pair_cannot_reactivate(void)
{
	struct tcpdelay_lifetime *lifetime = calloc(1U, sizeof(*lifetime));
	struct tcpdelay_estimator *estimator = calloc(1U, sizeof(*estimator));
	struct tcpdelay_record record;
	struct tcpdelay_lifetime_slot *slot;
	uint64_t departure = 0U;
	uint64_t first_generation;

	assert(lifetime != NULL && estimator != NULL);
	tcpdelay_lifetime_init(lifetime, TEST_EPOCH);
	tcpdelay_estimator_init(estimator);
	establish(lifetime, estimator, 10U, 100U, 200U);
	slot = find_flow(lifetime);
	assert(slot != NULL && slot->active);
	first_generation = slot->generation;
	record = record_for(
		20U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		201U,
		101U,
		1000U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(estimator->flows[0].generation == first_generation);

	/* New SYN withdraws old queue calibration and gates data pending its SYN/ACK. */
	record = record_for(
		30U,
		TCPDELAY_TCP_SYN,
		TEST_PACKET_OUTGOING,
		TEST_TIMESTAMP_ABSENT,
		300U,
		0U,
		0U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(!slot->active);
	assert(!estimator->flows[0].used);
	record = record_for(
		31U,
		TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		400U,
		300U,
		900U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(!slot->active && slot->pending[0] && slot->pending_isn[0] == 300U);
	assert(slot->generation == first_generation);
	record = record_for(
		31U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		301U,
		101U,
		1100U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(!estimator->flows[0].used);
	establish(lifetime, estimator, 40U, 300U, 400U);
	assert(slot->active);
	assert(slot->generation > first_generation);
	record = record_for(
		20U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		201U,
		101U,
		1200U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(!estimator->flows[0].used);
	record = record_for(
		42U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		401U,
		301U,
		1U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(estimator->flows[0].generation == slot->generation);
	assert(estimator->flows[0].last_tsval == 1U);
	record = record_for(
		50U,
		TCPDELAY_TCP_SYN,
		TEST_PACKET_OUTGOING,
		TEST_TIMESTAMP_ABSENT,
		100U,
		0U,
		0U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	establish(lifetime, estimator, 51U, 100U, 200U);
	assert(!slot->active);
	assert(!estimator->flows[0].used);
	free(estimator);
	free(lifetime);
}

static void test_departures_are_generation_qualified(void)
{
	struct tcpdelay_lifetime *lifetime = calloc(1U, sizeof(*lifetime));
	struct tcpdelay_estimator *estimator = calloc(1U, sizeof(*estimator));
	struct tcpdelay_record record;
	uint64_t departure = 0U;

	assert(lifetime != NULL && estimator != NULL);
	tcpdelay_lifetime_init(lifetime, TEST_EPOCH);
	tcpdelay_estimator_init(estimator);
	establish(lifetime, estimator, 100U, 500U, 600U);
	record = record_for(
		110U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_OUTGOING,
		TEST_TIMESTAMP_PRESENT,
		501U,
		601U,
		77U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	establish(lifetime, estimator, 120U, 700U, 800U);
	record = record_for(
		130U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		801U,
		701U,
		1000U,
		77U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(departure == 0U);
	free(estimator);
	free(lifetime);
}

static void test_duplicate_departure_keeps_first_timestamp(void)
{
	struct tcpdelay_lifetime *lifetime = calloc(1U, sizeof(*lifetime));
	struct tcpdelay_estimator *estimator = calloc(1U, sizeof(*estimator));
	struct tcpdelay_record record;
	uint64_t departure = 0U;

	assert(lifetime != NULL && estimator != NULL);
	tcpdelay_lifetime_init(lifetime, TEST_EPOCH);
	tcpdelay_estimator_init(estimator);
	establish(lifetime, estimator, 100U, 500U, 600U);
	record = record_for(
		110U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_OUTGOING,
		TEST_TIMESTAMP_PRESENT,
		501U,
		601U,
		77U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	record.arrival_ns = 111U;
	assert(add(lifetime, estimator, record, &departure));
	record = record_for(
		112U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		601U,
		501U,
		1000U,
		77U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(departure == 110U);
	free(estimator);
	free(lifetime);
}

static void test_stream_epoch_and_generation_exhaustion_fail_closed(void)
{
	struct tcpdelay_lifetime *lifetime = calloc(1U, sizeof(*lifetime));
	struct tcpdelay_estimator *estimator = calloc(1U, sizeof(*estimator));
	struct tcpdelay_record record;
	uint64_t departure = 0U;

	assert(lifetime != NULL && estimator != NULL);
	tcpdelay_lifetime_init(lifetime, TEST_EPOCH);
	tcpdelay_estimator_init(estimator);
	lifetime->next_generation = UINT32_MAX;
	record = record_for(
		1U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		1U,
		1U,
		1U,
		0U
	);
	assert(!add(lifetime, estimator, record, &departure));
	assert(lifetime->uncertain);
	tcpdelay_lifetime_reset(lifetime, TEST_EPOCH + 1U);
	assert(!add(lifetime, estimator, record, &departure));
	free(estimator);
	free(lifetime);
}

static void test_history_and_flow_capacity_fail_closed(void)
{
	struct tcpdelay_lifetime *lifetime = calloc(1U, sizeof(*lifetime));
	struct tcpdelay_estimator *estimator = calloc(1U, sizeof(*estimator));
	struct tcpdelay_lifetime_slot *slot;
	struct tcpdelay_record record;
	uint64_t departure = 0U;
	uint32_t index;

	assert(lifetime != NULL && estimator != NULL);
	tcpdelay_lifetime_init(lifetime, TEST_EPOCH);
	tcpdelay_estimator_init(estimator);
	for (index = 0U; index < TCPDELAY_LIFETIME_HISTORY + 1U; index++)
		establish(
			lifetime,
			estimator,
			100U + (uint64_t)index * 10U,
			1000U + index,
			2000U + index
		);
	slot = find_flow(lifetime);
	assert(slot != NULL && slot->history_count == TCPDELAY_LIFETIME_HISTORY);
	assert(!slot->active && slot->quarantined);
	assert(!lifetime->uncertain);

	tcpdelay_lifetime_reset(lifetime, TEST_EPOCH);
	for (index = 0U; index < TCPDELAY_LIFETIME_SLOTS; index++) {
		record = record_for(
			index + 1U,
			TCPDELAY_TCP_ACK,
			TEST_PACKET_INCOMING,
			TEST_TIMESTAMP_PRESENT,
			1U,
			1U,
			1U,
			0U
		);
		record.flow.local_port = (uint16_t)(1000U + index);
		assert(add(lifetime, estimator, record, &departure));
	}
	record = record_for(
		TCPDELAY_LIFETIME_SLOTS + 1U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		1U,
		1U,
		1U,
		0U
	);
	record.flow.local_port = UINT16_MAX;
	assert(!add(lifetime, estimator, record, &departure));
	assert(lifetime->uncertain);
	free(estimator);
	free(lifetime);
}

static void test_simultaneous_open_retransmission_and_sequence_wrap(void)
{
	struct tcpdelay_lifetime *lifetime = calloc(1U, sizeof(*lifetime));
	struct tcpdelay_estimator *estimator = calloc(1U, sizeof(*estimator));
	struct tcpdelay_lifetime_slot *slot;
	uint64_t departure = 0U;
	uint64_t generation;

	assert(lifetime != NULL && estimator != NULL);
	tcpdelay_lifetime_init(lifetime, TEST_EPOCH);
	tcpdelay_estimator_init(estimator);
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    10U,
			    TCPDELAY_TCP_SYN,
			    TEST_PACKET_OUTGOING,
			    TEST_TIMESTAMP_ABSENT,
			    10U,
			    0U,
			    0U,
			    0U
		    ),
		    &departure)
	);
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    11U,
			    TCPDELAY_TCP_SYN,
			    TEST_PACKET_INCOMING,
			    TEST_TIMESTAMP_ABSENT,
			    20U,
			    0U,
			    0U,
			    0U
		    ),
		    &departure)
	);
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    12U,
			    TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK,
			    TEST_PACKET_OUTGOING,
			    TEST_TIMESTAMP_PRESENT,
			    10U,
			    21U,
			    100U,
			    0U
		    ),
		    &departure)
	);
	slot = find_flow(lifetime);
	assert(slot != NULL && slot->active);
	generation = slot->generation;
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    13U,
			    TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK,
			    TEST_PACKET_INCOMING,
			    TEST_TIMESTAMP_PRESENT,
			    20U,
			    11U,
			    101U,
			    0U
		    ),
		    &departure)
	);
	assert(slot->generation == generation && slot->active);
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    14U,
			    TCPDELAY_TCP_SYN,
			    TEST_PACKET_OUTGOING,
			    TEST_TIMESTAMP_ABSENT,
			    10U,
			    0U,
			    0U,
			    0U
		    ),
		    &departure)
	);
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    15U,
			    TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK,
			    TEST_PACKET_INCOMING,
			    TEST_TIMESTAMP_PRESENT,
			    21U,
			    11U,
			    102U,
			    0U
		    ),
		    &departure)
	);
	assert(slot->generation > generation && slot->remote_isn == 21U);

	tcpdelay_lifetime_reset(lifetime, TEST_EPOCH);
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    20U,
			    TCPDELAY_TCP_SYN,
			    TEST_PACKET_OUTGOING,
			    TEST_TIMESTAMP_ABSENT,
			    UINT32_MAX,
			    0U,
			    0U,
			    0U
		    ),
		    &departure)
	);
	assert(
		add(lifetime,
		    estimator,
		    record_for(
			    21U,
			    TCPDELAY_TCP_SYN | TCPDELAY_TCP_ACK,
			    TEST_PACKET_INCOMING,
			    TEST_TIMESTAMP_PRESENT,
			    0U,
			    0U,
			    100U,
			    0U
		    ),
		    &departure)
	);
	slot = find_flow(lifetime);
	assert(slot != NULL && slot->active && slot->remote_isn == 0U);
	free(estimator);
	free(lifetime);
}

static void test_fin_rst_and_older_lifecycle_boundaries(void)
{
	struct tcpdelay_lifetime *lifetime = calloc(1U, sizeof(*lifetime));
	struct tcpdelay_estimator *estimator = calloc(1U, sizeof(*estimator));
	struct tcpdelay_lifetime_slot *slot;
	struct tcpdelay_record record;
	uint64_t departure = 0U;
	uint64_t generation;

	assert(lifetime != NULL && estimator != NULL);
	tcpdelay_lifetime_init(lifetime, TEST_EPOCH);
	tcpdelay_estimator_init(estimator);
	establish(lifetime, estimator, 100U, 300U, 400U);
	slot = find_flow(lifetime);
	assert(slot != NULL && slot->active);
	generation = slot->generation;
	record = record_for(
		99U,
		TCPDELAY_TCP_SYN,
		TEST_PACKET_OUTGOING,
		TEST_TIMESTAMP_ABSENT,
		900U,
		0U,
		0U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(slot->active && slot->generation == generation);
	record = record_for(
		110U,
		TCPDELAY_TCP_FIN | TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_ABSENT,
		401U,
		301U,
		0U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(!slot->active && slot->quarantined);
	record = record_for(
		111U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_PRESENT,
		402U,
		301U,
		1000U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(!estimator->flows[0].used);
	establish(lifetime, estimator, 120U, 500U, 600U);
	assert(slot->active);
	record = record_for(
		130U,
		TCPDELAY_TCP_RST | TCPDELAY_TCP_ACK,
		TEST_PACKET_OUTGOING,
		TEST_TIMESTAMP_PRESENT,
		501U,
		601U,
		1U,
		0U
	);
	assert(add(lifetime, estimator, record, &departure));
	assert(!slot->active && slot->quarantined);
	free(estimator);
	free(lifetime);
}

static void test_malformed_record_shape_is_uncertain(void)
{
	struct tcpdelay_lifetime *lifetime = calloc(1U, sizeof(*lifetime));
	struct tcpdelay_estimator *estimator = calloc(1U, sizeof(*estimator));
	struct tcpdelay_record record;
	uint64_t departure = 0U;

	assert(lifetime != NULL && estimator != NULL);
	tcpdelay_lifetime_init(lifetime, TEST_EPOCH);
	tcpdelay_estimator_init(estimator);
	record = record_for(
		1U,
		TCPDELAY_TCP_ACK,
		TEST_PACKET_INCOMING,
		TEST_TIMESTAMP_ABSENT,
		1U,
		1U,
		0U,
		0U
	);
	assert(!tcpdelay_record_valid(&record, TEST_EPOCH));
	assert(!add(lifetime, estimator, record, &departure));
	assert(lifetime->uncertain);
	free(estimator);
	free(lifetime);
}

int main(void)
{
	test_reuse_resets_and_old_pair_cannot_reactivate();
	test_departures_are_generation_qualified();
	test_duplicate_departure_keeps_first_timestamp();
	test_stream_epoch_and_generation_exhaustion_fail_closed();
	test_history_and_flow_capacity_fail_closed();
	test_simultaneous_open_retransmission_and_sequence_wrap();
	test_fin_rst_and_older_lifecycle_boundaries();
	test_malformed_record_shape_is_uncertain();
	puts("TCP lifetime stream tests passed");
	return 0;
}
