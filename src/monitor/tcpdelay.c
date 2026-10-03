#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/constants.h"
#include "common/error.h"
#include "config/defaults.h"
#include "logging/log.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

static bool open_capture(struct observation_context *context,
			 const struct monitored_direction *upload)
{
	char error[ERROR_SIZE] = { 0 };

	if (tcpdelay_capture_open(&context->tcp_capture, TCPDELAY_OBJECT_PATH, upload->interface,
				  &context->tcp_estimator, error, sizeof(error)) != 0) {
		log_message(LOG_LEVEL_WARNING, "TCP measurement degraded: %s", error);
		context->tcp_capture_failed_index = upload->cake.interface_index;
		return false;
	}
	/* Flows and windows from an earlier interface no longer apply. */
	tcpdelay_estimator_init(&context->tcp_estimator);
	context->tcp_capture_open = true;
	context->tcp_capture_failed_index = 0U;
	context->tcp_dropped_records = 0U;
	context->next_tcp_counter_check_microseconds = 0U;
	/* The counters restart with the new filter. */
	context->ack_sampled = false;
	context->ack_rate_valid = false;
	log_message(LOG_LEVEL_NOTICE, "TCP measurement started: interface=%s", upload->interface);
	return true;
}

static void report_dropped_records(struct observation_context *context,
				   uint64_t timestamp_microseconds)
{
	struct tcpdelay_counters counters;

	if (timestamp_microseconds < context->next_tcp_counter_check_microseconds)
		return;
	context->next_tcp_counter_check_microseconds =
		timestamp_microseconds + TCPDELAY_COUNTER_CHECK_MICROSECONDS;
	if (tcpdelay_capture_counters(&context->tcp_capture, &counters) != 0)
		return;
	if (counters.ring_full > context->tcp_dropped_records) {
		log_message(LOG_LEVEL_WARNING,
			    "TCP delay records dropped: %" PRIu64 " since the last check",
			    (uint64_t)(counters.ring_full - context->tcp_dropped_records));
		context->tcp_dropped_records = counters.ring_full;
	}
}

/* Opens, follows and drains the capture; false when it is not usable. */
static bool capture_ready(struct observation_context *context, const struct config *config,
			  uint64_t timestamp_microseconds)
{
	const struct monitored_direction *upload = &context->upload;

	if ((!config->tcp_delay_attribution && config->upload_ack_share_min_per_million == 0U) ||
	    upload->cake_state != CAKE_OBSERVATION_AVAILABLE) {
		return false;
	}
	if (context->tcp_capture_open &&
	    context->tcp_capture.interface_index != upload->cake.interface_index) {
		log_message(LOG_LEVEL_INFO, "TCP capture follows recreated interface=%s",
			    upload->interface);
		close_tcp_delay(context);
	}
	if (!context->tcp_capture_open &&
	    (context->tcp_capture_failed_index == upload->cake.interface_index ||
	     !open_capture(context, upload))) {
		return false;
	}
	/* Records wait in the ring buffer until drained, even without attribution. */
	if (tcpdelay_capture_drain(&context->tcp_capture) < 0) {
		log_message(LOG_LEVEL_WARNING, "TCP measurement degraded: capture failed: %s",
			    strerror(errno));
		close_tcp_delay(context);
		context->tcp_capture_failed_index = upload->cake.interface_index;
		return false;
	}
	report_dropped_records(context, timestamp_microseconds);
	return true;
}

static void measure_queues(struct observation_context *context, const struct config *config,
			   uint64_t timestamp_microseconds, struct controller_queue_input *queue)
{
	struct tcpdelay_estimate estimate;

	/* The filter timestamps with CLOCK_MONOTONIC, like the monitor. */
	tcpdelay_estimator_result(&context->tcp_estimator,
				  timestamp_microseconds * NANOSECONDS_PER_MICROSECOND, &estimate);
	if (config->output_processing_stats) {
		const struct log_tcp_queue_record record = {
			.download_valid = estimate.download_valid,
			.upload_valid = estimate.upload_valid,
			.download_queue_microseconds = estimate.download_queue_microseconds,
			.upload_queue_microseconds = estimate.upload_queue_microseconds
		};

		log_tcp_queue(&record);
	}
	queue->valid = estimate.download_valid && estimate.upload_valid;
	queue->download_microseconds = estimate.download_queue_microseconds;
	queue->upload_microseconds = estimate.upload_queue_microseconds;
}

/* Pure-ACK and total upload rates from the filter's byte counters, over >= 500 ms. */
static void measure_ack_rate(struct observation_context *context, uint64_t timestamp_microseconds,
			     struct controller_ack_input *acks)
{
	struct tcpdelay_counters counters;
	uint64_t elapsed;

	if (timestamp_microseconds >=
		    context->ack_sampled_microseconds + TCPDELAY_ACK_RATE_INTERVAL_MICROSECONDS &&
	    tcpdelay_capture_counters(&context->tcp_capture, &counters) == 0) {
		elapsed = timestamp_microseconds - context->ack_sampled_microseconds;
		if (context->ack_sampled && counters.ack_bytes >= context->ack_bytes &&
		    counters.upload_bytes >= context->upload_bytes) {
			context->ack_rate_bits_per_second =
				(counters.ack_bytes - context->ack_bytes) * BITS_PER_BYTE *
				MICROSECONDS_PER_SECOND / elapsed;
			context->upload_rate_bits_per_second =
				(counters.upload_bytes - context->upload_bytes) * BITS_PER_BYTE *
				MICROSECONDS_PER_SECOND / elapsed;
			context->ack_rate_valid = true;
		}
		context->ack_bytes = counters.ack_bytes;
		context->upload_bytes = counters.upload_bytes;
		context->ack_sampled_microseconds = timestamp_microseconds;
		context->ack_sampled = true;
	}
	acks->valid = context->ack_rate_valid;
	acks->upload_ack_rate_bits_per_second = context->ack_rate_bits_per_second;
	acks->upload_rate_bits_per_second = context->upload_rate_bits_per_second;
}

/*
 * Drains the capture on demand, so the estimates are current whenever the
 * controller runs. Records are submitted without wakeups and simply wait in
 * the ring buffer between latency samples.
 */
void observe_tcp_capture(struct observation_context *context, const struct config *config,
			 uint64_t timestamp_microseconds, struct controller_queue_input *queue,
			 struct controller_ack_input *acks)
{
	queue->valid = false;
	acks->valid = false;
	if (!capture_ready(context, config, timestamp_microseconds))
		return;
	if (config->tcp_delay_attribution)
		measure_queues(context, config, timestamp_microseconds, queue);
	if (config->upload_ack_share_min_per_million != 0U)
		measure_ack_rate(context, timestamp_microseconds, acks);
}

void close_tcp_delay(struct observation_context *context)
{
	if (!context->tcp_capture_open)
		return;
	tcpdelay_capture_close(&context->tcp_capture);
	context->tcp_capture_open = false;
}
