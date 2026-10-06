#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/constants.h"
#include "common/error.h"
#include "common/utils.h"
#include "config/defaults.h"
#include "logging/log.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

static struct cake_accounting accounting_model(const struct cake_observation *cake)
{
	return (struct cake_accounting){
		.overhead_bytes = cake->overhead_bytes,
		.mpu_bytes = cake->mpu_bytes,
		.atm_mode = cake->atm_mode,
		.raw = cake->raw ? 1U : 0U,
	};
}

/* Either TCP feature needs the capture. */
static bool tcp_enabled(const struct config *config)
{
	return config->tcp_delay_attribution || config->ul_congest_ack_share_per_million != 0U;
}

void tcp_init(struct monitor *monitor)
{
	tcpdelay_capture_init(&monitor->tcp.capture);
}

void tcp_start(struct monitor *monitor)
{
	char error[ERROR_SIZE] = { 0 };

	if (tcp_enabled(monitor->config) &&
	    tcpdelay_capture_load(&monitor->tcp.capture, error, sizeof(error)) != 0)
		log_message(LOG_LEVEL_WARNING, "TCP measurement degraded: %s", error);
}

static bool open_capture(struct monitor *monitor, const struct monitor_direction *upload)
{
	const struct cake_observation *cake = &upload->cake;
	struct monitor_tcp *tcp = &monitor->tcp;
	char error[ERROR_SIZE] = { 0 };
	const struct cake_accounting model = accounting_model(cake);
	const struct cake_accounting *accounting =
		monitor->config->ul_congest_ack_share_per_million != 0U ? &model : NULL;

	if (tcpdelay_capture_open(
		    &tcp->capture,
		    upload->interface,
		    accounting,
		    error,
		    sizeof(error)
	    ) != 0) {
		log_message(LOG_LEVEL_WARNING, "TCP measurement degraded: %s", error);
		tcp->failed_index = cake->qdisc.interface_index;
		return false;
	}
	tcp->open = true;
	tcp->qdisc = cake->qdisc;
	tcp->failed_index = 0U;
	tcp->dropped_records = 0U;
	tcp->next_counter_check_microseconds = 0U;
	/* The counters restart with the new filter. */
	tcp->ack_sampled = false;
	tcp->ack_rate_valid = false;
	tcp->ack_degraded = false;
	tcp->unaccounted_packets = 0U;
	log_message(LOG_LEVEL_NOTICE, "TCP measurement started: interface=%s", upload->interface);
	return true;
}

static void report_dropped_records(struct monitor *monitor, uint64_t timestamp_microseconds)
{
	struct monitor_tcp *tcp = &monitor->tcp;
	struct tcpdelay_counters counters;

	if (timestamp_microseconds < tcp->next_counter_check_microseconds)
		return;
	tcp->next_counter_check_microseconds =
		timestamp_microseconds + TCPDELAY_COUNTER_CHECK_MICROSECONDS;
	if (tcpdelay_capture_counters(&tcp->capture, &counters) != 0)
		return;
	if (counters.ring_full > tcp->dropped_records) {
		log_message(
			LOG_LEVEL_WARNING,
			"TCP delay records dropped: %" PRIu64 " since the last check",
			(uint64_t)(counters.ring_full - tcp->dropped_records)
		);
		tcp->dropped_records = counters.ring_full;
	}
}

/* Adds pending records to the estimator; false when the capture failed and was closed. */
static bool drain(struct monitor *monitor)
{
	struct monitor_tcp *tcp = &monitor->tcp;

	if (tcpdelay_capture_drain(&tcp->capture) >= 0)
		return true;
	log_message(
		LOG_LEVEL_WARNING,
		"TCP measurement degraded: capture failed: %s",
		strerror(errno)
	);
	tcp_close(monitor);
	/* Not retried until the upload interface the capture was bound to is recreated. */
	tcp->failed_index = tcp->qdisc.interface_index;
	return false;
}

/* Opens the capture, or reopens it for a changed qdisc; false when it is not usable. */
static bool capture_ready(struct monitor *monitor)
{
	struct monitor_tcp *tcp = &monitor->tcp;
	const struct config *config = monitor->config;
	const struct monitor_direction *upload = &monitor->links.upload;
	const struct cake_observation *cake = &upload->cake;
	const struct cake_accounting model = accounting_model(cake);

	if (!tcp_enabled(config) || upload->cake_state != CAKE_OBSERVATION_AVAILABLE) {
		tcp_close(monitor);
		return false;
	}
	if (tcp->open && (!qdisc_same(&tcp->qdisc, &cake->qdisc) ||
			  (tcp->capture.accounting.enabled &&
			   memcmp(&tcp->capture.accounting.cake, &model, sizeof(model)) != 0))) {
		log_message(
			LOG_LEVEL_INFO,
			"TCP capture follows changed interface or CAKE accounting: interface=%s",
			upload->interface
		);
		tcp_close(monitor);
	}
	if (tcp->open)
		return true;
	return tcp->failed_index != cake->qdisc.interface_index && open_capture(monitor, upload);
}

static void measure_queues(
	struct monitor *monitor,
	uint64_t timestamp_microseconds,
	struct controller_queue_input *queue
)
{
	struct monitor_tcp *tcp = &monitor->tcp;
	struct tcpdelay_estimate estimate;

	/* The filter timestamps with CLOCK_MONOTONIC, like the monitor. */
	tcpdelay_estimator_result(
		&tcp->capture.estimator,
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
	return mul_div(
		saturating_mul(bytes - previous_bytes, BITS_PER_BYTE),
		MICROSECONDS_PER_SECOND,
		elapsed
	);
}

static void ack_accounting_state(struct monitor *monitor, bool degraded)
{
	struct monitor_tcp *tcp = &monitor->tcp;

	if (tcp->ack_degraded == degraded)
		return;
	tcp->ack_degraded = degraded;
	if (degraded)
		log_message(
			LOG_LEVEL_WARNING,
			"ACK ceiling disabled: incomplete CAKE accounting or unavailable counters"
		);
	else
		log_message(LOG_LEVEL_NOTICE, "ACK accounting recovered");
}

/* Pure-ACK and total upload rates from the filter's byte counters, over >= 500 ms. */
static void measure_ack_rate(
	struct monitor *monitor,
	uint64_t timestamp_microseconds,
	struct controller_ack_input *acks
)
{
	struct monitor_tcp *tcp = &monitor->tcp;
	struct tcpdelay_counters counters;
	uint64_t elapsed;

	if (timestamp_microseconds >=
	    tcp->ack_sampled_microseconds + TCPDELAY_ACK_RATE_INTERVAL_MICROSECONDS) {
		bool degraded;

		if (tcpdelay_capture_counters(&tcp->capture, &counters) != 0) {
			tcp->ack_rate_valid = false;
			tcp->ack_sampled = false;
			tcp->ack_sampled_microseconds = timestamp_microseconds;
			degraded = true;
			ack_accounting_state(monitor, degraded);
			goto result;
		}
		elapsed = timestamp_microseconds - tcp->ack_sampled_microseconds;
		degraded = counters.unaccounted_packets != tcp->unaccounted_packets ||
			   (tcp->ack_sampled && (counters.ack_bytes < tcp->ack_bytes ||
						 counters.upload_bytes < tcp->upload_bytes));
		tcp->ack_rate_valid = false;
		if (tcp->ack_sampled && !degraded) {
			tcp->ack_rate_bits_per_second =
				rate_since(counters.ack_bytes, tcp->ack_bytes, elapsed);
			tcp->upload_rate_bits_per_second =
				rate_since(counters.upload_bytes, tcp->upload_bytes, elapsed);
			tcp->ack_rate_valid = true;
		}
		/* A baseline alone cannot establish recovery after unavailable counters. */
		if (degraded || tcp->ack_rate_valid)
			ack_accounting_state(monitor, degraded);
		tcp->ack_bytes = counters.ack_bytes;
		tcp->upload_bytes = counters.upload_bytes;
		tcp->unaccounted_packets = counters.unaccounted_packets;
		tcp->ack_sampled_microseconds = timestamp_microseconds;
		tcp->ack_sampled = true;
	}
result:
	acks->valid = tcp->ack_rate_valid;
	acks->upload_ack_rate_bits_per_second = tcp->ack_rate_bits_per_second;
	acks->upload_rate_bits_per_second = tcp->upload_rate_bits_per_second;
}

/*
 * Drains the capture on demand, so the estimates are current whenever the
 * controller runs. Records are submitted without wakeups and wait in the ring
 * buffer until this or the next traffic tick drains them.
 */
void tcp_observe(struct monitor *monitor, struct controller_input *input)
{
	const struct controller_latency_input *download = &input->download_latency;
	const struct controller_latency_input *upload = &input->upload_latency;
	const struct config *config = monitor->config;
	int64_t round_trip = download->owd_delta_microseconds + upload->owd_delta_microseconds;

	input->queue.valid = false;
	input->acks.valid = false;
	if (!capture_ready(monitor))
		return;
	/*
	 * A queue on the access link delays fping's round trip as well, so no TCP
	 * queue may exceed fping's added delay. The bound stays until the next
	 * reply, also for records the traffic tick drains.
	 */
	if (download->valid && upload->valid)
		tcpdelay_estimator_set_bound(
			&monitor->tcp.capture.estimator,
			max_i64(round_trip, 0) * (int64_t)NANOSECONDS_PER_MICROSECOND
		);
	/* Records wait in the ring buffer until drained, even without attribution. */
	if (!drain(monitor))
		return;
	report_dropped_records(monitor, input->timestamp_microseconds);
	if (config->tcp_delay_attribution)
		measure_queues(monitor, input->timestamp_microseconds, &input->queue);
	if (config->ul_congest_ack_share_per_million != 0U)
		measure_ack_rate(monitor, input->timestamp_microseconds, &input->acks);
}

/*
 * The controller drains the ring on every ping reply; the traffic tick drains
 * it too, so records do not overflow while no reply runs the controller, as
 * when pingers are stopped in IDLE or while CAKE is missing.
 */
void tcp_drain(struct monitor *monitor)
{
	struct monitor_direction *upload = &monitor->links.upload;
	struct monitor_tcp *tcp = &monitor->tcp;

	if (upload->cake_state != CAKE_OBSERVATION_AVAILABLE) {
		tcp_close(monitor);
		return;
	}
	if (tcp->open)
		(void)drain(monitor);
}

void tcp_close(struct monitor *monitor)
{
	struct monitor_tcp *tcp = &monitor->tcp;

	if (!tcp->open)
		return;
	tcpdelay_capture_close(&tcp->capture);
	tcp->open = false;
	tcp->ack_sampled = false;
	tcp->ack_rate_valid = false;
}

void tcp_stop(struct monitor *monitor)
{
	tcp_close(monitor);
	tcpdelay_capture_unload(&monitor->tcp.capture);
}
