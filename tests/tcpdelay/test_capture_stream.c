/* Exercise capture stream loss detection and epoch recovery without BPF syscalls. */
#define _GNU_SOURCE

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/bpf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/helpers.h"

static int test_map_lookup(int descriptor, const void *key, void *value);
static int test_map_update(int descriptor, const void *key, const void *value, __u64 flags);
static int test_consume_n(struct ring_buffer *ring, size_t count);
static struct ring *test_ring(const struct ring_buffer *buffer, unsigned int index);
static size_t test_avail(const struct ring *ring);
static bool test_clock(clockid_t clock_identifier, uint64_t *timestamp);

#define bpf_map_lookup_elem test_map_lookup
#define bpf_map_update_elem test_map_update
#define ring_buffer__consume_n test_consume_n
#define ring_buffer__ring test_ring
#define ring__avail_data_size test_avail
#define read_clock_microseconds test_clock
#include "tcpdelay/capture.c"

#include <assert.h>
#include <stdio.h>

#define EPOCH_DESCRIPTOR 1
#define LOSS_DESCRIPTOR 2
#define FAULT_DESCRIPTOR 3

static uint32_t map_epoch = 1U;
static uint32_t lost_epoch;
static uint8_t fault_storage[16];
static uint64_t current_microseconds;
static size_t available_bytes;
static size_t consumed_records;
static size_t loss_reads;
static bool epoch_read_failure;
static bool loss_read_failure;
static bool appear_loss_after_first_read;
static bool update_failure;

static int test_map_lookup(int descriptor, const void *key, void *value)
{
	if (descriptor == EPOCH_DESCRIPTOR) {
		if (epoch_read_failure) {
			errno = EIO;
			return -1;
		}
		assert(*(const uint32_t *)key == 0U);
		*(uint32_t *)value = map_epoch;
		return 0;
	}
	if (descriptor == LOSS_DESCRIPTOR) {
		uint32_t epoch = *(const uint32_t *)key;

		loss_reads++;
		if (loss_read_failure) {
			errno = EIO;
			return -1;
		}
		if (epoch == lost_epoch || (appear_loss_after_first_read && loss_reads > 1U)) {
			*(uint32_t *)value = 1U;
			return 0;
		}
		errno = ENOENT;
		return -1;
	}
	assert(descriptor == FAULT_DESCRIPTOR && *(const uint32_t *)key == 0U);
	memcpy(value, fault_storage, sizeof(fault_storage));
	return 0;
}

static int test_map_update(int descriptor, const void *key, const void *value, __u64 flags)
{
	(void)flags;
	assert(descriptor == EPOCH_DESCRIPTOR && *(const uint32_t *)key == 0U);
	if (update_failure) {
		errno = EIO;
		return -1;
	}
	map_epoch = *(const uint32_t *)value;
	return 0;
}

static int test_consume_n(struct ring_buffer *ring, size_t count)
{
	assert(ring == (struct ring_buffer *)1);
	assert(count == TCPDELAY_STREAM_MAX_DRAIN);
	consumed_records++;
	return 0;
}

static struct ring *test_ring(const struct ring_buffer *buffer, unsigned int index)
{
	assert(buffer == (struct ring_buffer *)1 && index == 0U);
	return (struct ring *)1;
}

static size_t test_avail(const struct ring *ring)
{
	assert(ring == (struct ring *)1);
	return available_bytes;
}

static bool test_clock(clockid_t clock_identifier, uint64_t *timestamp)
{
	assert(clock_identifier == CLOCK_MONOTONIC);
	*timestamp = current_microseconds;
	return true;
}

static void setup(struct tcpdelay_capture *capture, struct tcpdelay_estimator *estimator)
{
	memset(capture, 0, sizeof(*capture));
	capture->ring = (struct ring_buffer *)1;
	capture->epoch_descriptor = EPOCH_DESCRIPTOR;
	capture->loss_descriptor = LOSS_DESCRIPTOR;
	capture->fault_descriptor = FAULT_DESCRIPTOR;
	capture->epoch = 1U;
	capture->state = TCPDELAY_CAPTURE_READY;
	capture->cpu_count = 2U;
	capture->fault_values = calloc(2U, PERCPU_VALUE_STRIDE);
	capture->lifetime = calloc(1U, sizeof(*capture->lifetime));
	capture->estimator = estimator;
	assert(capture->fault_values != NULL && capture->lifetime != NULL);
	tcpdelay_estimator_init(estimator);
	tcpdelay_lifetime_init(capture->lifetime, capture->epoch);
	map_epoch = 1U;
	lost_epoch = 0U;
	memset(fault_storage, 0, sizeof(fault_storage));
	current_microseconds = 2U * SECOND;
	available_bytes = 0U;
	consumed_records = 0U;
	loss_reads = 0U;
	epoch_read_failure = false;
	loss_read_failure = false;
	appear_loss_after_first_read = false;
	update_failure = false;
}

static void teardown(struct tcpdelay_capture *capture)
{
	free(capture->fault_values);
	free(capture->lifetime);
}

static void test_enoent_backlog_and_post_drain_loss(void)
{
	struct tcpdelay_capture capture;
	struct tcpdelay_estimator estimator;

	setup(&capture, &estimator);
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(tcpdelay_capture_timing_available(&capture));
	available_bytes = 1U;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_READY);
	assert(!tcpdelay_capture_timing_available(&capture));
	available_bytes = 0U;
	loss_reads = 0U;
	appear_loss_after_first_read = true;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_RECOVERING);
	assert(!tcpdelay_capture_timing_available(&capture));
	teardown(&capture);
}

static void test_cpu_stride_and_sticky_fatal(void)
{
	struct tcpdelay_capture capture;
	struct tcpdelay_estimator estimator;
	uint32_t fatal = 1U;

	setup(&capture, &estimator);
	memcpy(fault_storage + PERCPU_VALUE_STRIDE, &fatal, sizeof(fatal));
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_DISABLED);
	assert(!tcpdelay_capture_timing_available(&capture));
	memset(fault_storage, 0, sizeof(fault_storage));
	current_microseconds += TCPDELAY_RECOVERY_COOLDOWN_MICROSECONDS;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_DISABLED);
	teardown(&capture);
}

static void test_status_failure_and_epoch_exhaustion(void)
{
	struct tcpdelay_capture capture;
	struct tcpdelay_estimator estimator;

	setup(&capture, &estimator);
	epoch_read_failure = true;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_DISABLED);
	teardown(&capture);

	setup(&capture, &estimator);
	map_epoch = 2U;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_DISABLED);
	teardown(&capture);

	setup(&capture, &estimator);
	loss_read_failure = true;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_DISABLED);
	teardown(&capture);

	setup(&capture, &estimator);
	capture.epoch = TCPDELAY_STREAM_EPOCHS;
	map_epoch = capture.epoch;
	capture.state = TCPDELAY_CAPTURE_RECOVERING;
	capture.retry_after_microseconds = 0U;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_DISABLED);
	teardown(&capture);
}

static void test_epoch_update_failure_disables_timing(void)
{
	struct tcpdelay_capture capture;
	struct tcpdelay_estimator estimator;
	struct tcpdelay_record record = {
		.arrival_ns = 1U,
		.metadata = TCPDELAY_TCP_ACK |
			    (TCPDELAY_STREAM_VERSION << TCPDELAY_STREAM_VERSION_SHIFT) |
			    (1U << TCPDELAY_STREAM_EPOCH_SHIFT),
	};

	setup(&capture, &estimator);
	assert(add_record(&capture, &record, sizeof(record)) == 0);
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_RECOVERING);
	current_microseconds += TCPDELAY_RECOVERY_COOLDOWN_MICROSECONDS;
	update_failure = true;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_DISABLED);
	assert(capture.epoch == 1U && map_epoch == 1U);
	teardown(&capture);
}

static void test_record_shape_and_epoch_publication(void)
{
	struct tcpdelay_capture capture;
	struct tcpdelay_estimator estimator;
	struct tcpdelay_record record = {
		.arrival_ns = 1U,
		.metadata = TCPDELAY_TCP_ACK |
			    (TCPDELAY_STREAM_VERSION << TCPDELAY_STREAM_VERSION_SHIFT) |
			    (1U << TCPDELAY_STREAM_EPOCH_SHIFT),
	};

	setup(&capture, &estimator);
	assert(add_record(&capture, &record, sizeof(record)) == 0);
	assert(capture.uncertain_pending);
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_RECOVERING);
	current_microseconds += TCPDELAY_RECOVERY_COOLDOWN_MICROSECONDS;
	loss_reads = 0U;
	assert(tcpdelay_capture_drain(&capture) == 0);
	assert(capture.state == TCPDELAY_CAPTURE_READY && capture.epoch == 2U);
	assert(map_epoch == 2U && capture.lifetime->epoch == 2U);
	teardown(&capture);
}

int main(void)
{
	test_enoent_backlog_and_post_drain_loss();
	test_cpu_stride_and_sticky_fatal();
	test_status_failure_and_epoch_exhaustion();
	test_record_shape_and_epoch_publication();
	test_epoch_update_failure_disables_timing();
	puts("TCP capture stream tests passed");
	return 0;
}
