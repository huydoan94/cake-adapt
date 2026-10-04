#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/constants.h"
#include "common/error.h"
#include "config/defaults.h"
#include "logging/log.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

static bool open_capture(struct monitor *monitor, const struct monitor_direction *upload)
{
	char error[ERROR_SIZE] = { 0 };

	if (tcpdelay_capture_open(
		    &monitor->tcp.capture,
		    TCPDELAY_OBJECT_PATH,
		    upload->interface,
		    &monitor->tcp.estimator,
		    error,
		    sizeof(error)
	    ) != 0) {
		log_message(LOG_LEVEL_WARNING, "TCP measurement degraded: %s", error);
		monitor->tcp.failed_index = upload->cake.interface_index;
		return false;
	}
	/* Flows and windows from an earlier interface no longer apply. */
	tcpdelay_estimator_init(&monitor->tcp.estimator);
	monitor->tcp.open = true;
	monitor->tcp.failed_index = 0U;
	monitor->tcp.dropped_records = 0U;
	monitor->tcp.next_counter_check_microseconds = 0U;
	/* The counters restart with the new filter. */
	monitor->tcp.ack_sampled = false;
	monitor->tcp.ack_rate_valid = false;
	log_message(LOG_LEVEL_NOTICE, "TCP measurement started: interface=%s", upload->interface);
	return true;
}

static void report_dropped_records(struct monitor *monitor, uint64_t timestamp_microseconds)
{
	struct tcpdelay_counters counters;

	if (timestamp_microseconds < monitor->tcp.next_counter_check_microseconds)
		return;
	monitor->tcp.next_counter_check_microseconds =
		timestamp_microseconds + TCPDELAY_COUNTER_CHECK_MICROSECONDS;
	if (tcpdelay_capture_counters(&monitor->tcp.capture, &counters) != 0)
		return;
	if (counters.ring_full > monitor->tcp.dropped_records) {
		log_message(
			LOG_LEVEL_WARNING,
			"TCP delay records dropped: %" PRIu64 " since the last check",
			(uint64_t)(counters.ring_full - monitor->tcp.dropped_records)
		);
		monitor->tcp.dropped_records = counters.ring_full;
	}
}

/* Adds pending records to the estimator; false when the capture failed and was closed. */
static bool drain(struct monitor *monitor)
{
	if (tcpdelay_capture_drain(&monitor->tcp.capture) >= 0)
		return true;
	log_message(
		LOG_LEVEL_WARNING,
		"TCP measurement degraded: capture failed: %s",
		strerror(errno)
	);
	tcp_close(monitor);
	monitor->tcp.failed_index = monitor->links.upload.cake.interface_index;
	return false;
}

/* Opens, follows and drains the capture; false when it is not usable. */
static bool capture_ready(struct monitor *monitor, uint64_t timestamp_microseconds)
{
	const struct config *config = monitor->config;
	const struct monitor_direction *upload = &monitor->links.upload;

	if ((!config->tcp_delay_attribution &&
	     config->upload_ack_congested_share_per_million == 0U) ||
	    upload->cake_state != CAKE_OBSERVATION_AVAILABLE) {
		return false;
	}
	if (monitor->tcp.open &&
	    monitor->tcp.capture.interface_index != upload->cake.interface_index) {
		log_message(
			LOG_LEVEL_INFO,
			"TCP capture follows recreated interface=%s",
			upload->interface
		);
		tcp_close(monitor);
	}
	if (!monitor->tcp.open && (monitor->tcp.failed_index == upload->cake.interface_index ||
				   !open_capture(monitor, upload))) {
		return false;
	}
	/* Records wait in the ring buffer until drained, even without attribution. */
	if (!drain(monitor))
		return false;
	report_dropped_records(monitor, timestamp_microseconds);
	return true;
}

static void measure_queues(
	struct monitor *monitor,
	uint64_t timestamp_microseconds,
	struct controller_queue_input *queue
)
{
	struct tcpdelay_estimate estimate;

	/* The filter timestamps with CLOCK_MONOTONIC, like the monitor. */
	tcpdelay_estimator_result(
		&monitor->tcp.estimator,
		timestamp_microseconds * NANOSECONDS_PER_MICROSECOND,
		&estimate
	);
	if (monitor->config->output_processing_stats) {
		const struct log_tcp_queue_record record = {
			.download_valid = estimate.download_valid,
			.upload_valid = estimate.upload_valid,
			.download_queue_microseconds = estimate.download_queue_microseconds,
			.upload_queue_microseconds = estimate.upload_queue_microseconds,
		};

		log_tcp_queue(&record);
	}
	queue->valid = estimate.download_valid && estimate.upload_valid;
	queue->download_microseconds = estimate.download_queue_microseconds;
	queue->upload_microseconds = estimate.upload_queue_microseconds;
}

/* elapsed is at least TCPDELAY_ACK_RATE_INTERVAL_MICROSECONDS, so never zero. */
static uint64_t rate_since(uint64_t bytes, uint64_t previous_bytes, uint64_t elapsed)
{
	return (bytes - previous_bytes) * BITS_PER_BYTE * MICROSECONDS_PER_SECOND / elapsed;
}

/* Pure-ACK and total upload rates from the filter's byte counters, over >= 500 ms. */
static void measure_ack_rate(
	struct monitor *monitor,
	uint64_t timestamp_microseconds,
	struct controller_ack_input *acks
)
{
	struct tcpdelay_counters counters;
	uint64_t elapsed;

	if (timestamp_microseconds >= monitor->tcp.ack_sampled_microseconds +
					      TCPDELAY_ACK_RATE_INTERVAL_MICROSECONDS &&
	    tcpdelay_capture_counters(&monitor->tcp.capture, &counters) == 0) {
		elapsed = timestamp_microseconds - monitor->tcp.ack_sampled_microseconds;
		if (monitor->tcp.ack_sampled && counters.ack_bytes >= monitor->tcp.ack_bytes &&
		    counters.upload_bytes >= monitor->tcp.upload_bytes) {
			monitor->tcp.ack_rate_bits_per_second =
				rate_since(counters.ack_bytes, monitor->tcp.ack_bytes, elapsed);
			monitor->tcp.upload_rate_bits_per_second = rate_since(
				counters.upload_bytes,
				monitor->tcp.upload_bytes,
				elapsed
			);
			monitor->tcp.ack_rate_valid = true;
		}
		monitor->tcp.ack_bytes = counters.ack_bytes;
		monitor->tcp.upload_bytes = counters.upload_bytes;
		monitor->tcp.ack_sampled_microseconds = timestamp_microseconds;
		monitor->tcp.ack_sampled = true;
	}
	acks->valid = monitor->tcp.ack_rate_valid;
	acks->upload_ack_rate_bits_per_second = monitor->tcp.ack_rate_bits_per_second;
	acks->upload_rate_bits_per_second = monitor->tcp.upload_rate_bits_per_second;
}

/*
 * Drains the capture on demand, so the estimates are current whenever the
 * controller runs. Records are submitted without wakeups and wait in the ring
 * buffer until this or the next traffic tick drains them.
 */
void tcp_observe(
	struct monitor *monitor,
	uint64_t timestamp_microseconds,
	struct controller_queue_input *queue,
	struct controller_ack_input *acks
)
{
	const struct config *config = monitor->config;

	queue->valid = false;
	acks->valid = false;
	if (!capture_ready(monitor, timestamp_microseconds))
		return;
	if (config->tcp_delay_attribution)
		measure_queues(monitor, timestamp_microseconds, queue);
	if (config->upload_ack_congested_share_per_million != 0U)
		measure_ack_rate(monitor, timestamp_microseconds, acks);
}

/*
 * The controller drains the ring on every ping reply; the traffic tick drains
 * it too, so records do not overflow while no reply runs the controller, as
 * when pingers are stopped in IDLE or while CAKE is missing.
 */
void tcp_drain(struct monitor *monitor)
{
	if (monitor->tcp.open)
		(void)drain(monitor);
}

void tcp_close(struct monitor *monitor)
{
	if (!monitor->tcp.open)
		return;
	tcpdelay_capture_close(&monitor->tcp.capture);
	monitor->tcp.open = false;
}
