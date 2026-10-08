#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/constants.h"
#include "common/error.h"
#include "common/helpers.h"
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
	return config->tcp_delay_attribution || config->ul_congest_ack_share_ratio_e6 != 0U;
}

void tcp_init(struct monitor *monitor)
{
	tcpdelay_capture_init(&monitor->tcp.capture);
	tcpdelay_injector_init(&monitor->tcp.injector);
}

void tcp_start(struct monitor *monitor)
{
	char error[ERROR_SIZE] = { 0 };

	/* The injector shares the filter's object; validation requires attribution. */
	if (monitor->config->tcp_timestamp_inject)
		tcpdelay_capture_enable_injection(&monitor->tcp.capture);
	if (tcp_enabled(monitor->config) &&
	    tcpdelay_capture_load(&monitor->tcp.capture, error, sizeof(error)) != 0)
		log_message(LOG_LEVEL_WARNING, "TCP measurement degraded: %s", error);
}

static struct log_tcp_inject_record inject_record(const struct tcpdelay_inject_counters *counters)
{
	return (struct log_tcp_inject_record){
		.injected = counters->injected,
		.skipped = counters->skipped,
		.server_accepted = counters->server_accepted,
		.server_declined = counters->server_declined,
		.client_rejected = counters->client_rejected,
		.server_rejected = counters->server_rejected,
		.retried = counters->retried,
		.failed = counters->failed,
		.stalled = counters->stalled,
	};
}

/* Experimental: follows the capture onto the same interface, unless paused. */
static void attach_injector(struct monitor *monitor, const char *interface)
{
	char error[ERROR_SIZE] = { 0 };

	if (!monitor->config->tcp_timestamp_inject || monitor->tcp.inject_breaker.paused)
		return;
	if (tcpdelay_injector_attach(
		    &monitor->tcp.injector,
		    &monitor->tcp.capture,
		    interface,
		    error,
		    sizeof(error)
	    ) != 0) {
		log_message(LOG_LEVEL_WARNING, "TCP timestamp injection disabled: %s", error);
		return;
	}
	log_message(
		LOG_LEVEL_NOTICE,
		"TCP timestamp injection started (experimental): interface=%s",
		interface
	);
}

static void detach_injector(struct monitor *monitor)
{
	struct tcpdelay_injector *injector = &monitor->tcp.injector;
	struct tcpdelay_inject_counters counters;

	if (!tcpdelay_injector_attached(injector))
		return;
	if (tcpdelay_injector_counters(injector, &counters) == 0)
		log_message(
			LOG_LEVEL_NOTICE,
			"TCP timestamp injection stopped: injected=%" PRIu64
			" server_accepted=%" PRIu64 " server_declined=%" PRIu64
			" client_rejected=%" PRIu64 " server_rejected=%" PRIu64 " skipped=%" PRIu64
			" failed=%" PRIu64 " stalled=%" PRIu64,
			(uint64_t)counters.injected,
			(uint64_t)counters.server_accepted,
			(uint64_t)counters.server_declined,
			(uint64_t)counters.client_rejected,
			(uint64_t)counters.server_rejected,
			(uint64_t)counters.skipped,
			(uint64_t)counters.failed,
			(uint64_t)counters.stalled
		);
	tcpdelay_injector_detach(injector);
}

static bool open_capture(struct monitor *monitor, const struct monitor_direction *upload)
{
	const struct cake_observation *cake = &upload->cake;
	struct monitor_tcp *tcp = &monitor->tcp;
	char error[ERROR_SIZE] = { 0 };
	const struct cake_accounting model = accounting_model(cake);
	const struct cake_accounting *accounting =
		monitor->config->ul_congest_ack_share_ratio_e6 != 0U ? &model : NULL;

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
	tcp->next_counter_check_us = 0U;
	/* The counters restart with the new filter. */
	tcp->ack_sampled = false;
	tcp->ack_rate_valid = false;
	tcp->ack_degraded = false;
	tcp->unaccounted_packets = 0U;
	log_message(LOG_LEVEL_NOTICE, "TCP measurement started: interface=%s", upload->interface);
	attach_injector(monitor, upload->interface);
	return true;
}

/*
 * Once a minute: logs the injector's counters and pauses it for a day when
 * its handshakes stall repeatedly, or attaches it again after the pause.
 */
static void check_injector(struct monitor *monitor, uint64_t timestamp_us)
{
	struct monitor_tcp *tcp = &monitor->tcp;
	struct tcpdelay_inject_counters counters;

	if (tcpdelay_injector_counters(&tcp->injector, &counters) != 0)
		return;
	if (monitor->config->output_processing_stats &&
	    tcpdelay_injector_attached(&tcp->injector)) {
		const struct log_tcp_inject_record record = inject_record(&counters);

		log_tcp_inject(&record);
	}
	switch (
		tcpdelay_inject_breaker_check(&tcp->inject_breaker, counters.stalled, timestamp_us)
	) {
	case TCPDELAY_INJECT_BREAKER_PAUSE:
		log_message(
			LOG_LEVEL_WARNING,
			"TCP timestamp injection paused for 24 hours: handshakes stalled=%" PRIu64
			"; a client's TCP timestamp clock looks older than the injected one, as on"
			" Windows up longer than 24.8 days; restart such PCs, or enable timestamps on"
			" them (netsh int tcp set global timestamps=enabled)",
			(uint64_t)counters.stalled
		);
		detach_injector(monitor);
		break;
	case TCPDELAY_INJECT_BREAKER_RESUME:
		attach_injector(monitor, monitor->links.upload.interface);
		break;
	case TCPDELAY_INJECT_BREAKER_KEEP:
		break;
	}
}

static void report_dropped_records(struct monitor *monitor, uint64_t timestamp_us)
{
	struct monitor_tcp *tcp = &monitor->tcp;
	struct tcpdelay_counters counters;

	if (timestamp_us < tcp->next_counter_check_us)
		return;
	tcp->next_counter_check_us = timestamp_us + TCPDELAY_COUNTER_CHECK_US;
	if (tcpdelay_injector_attached(&tcp->injector) || tcp->inject_breaker.paused)
		check_injector(monitor, timestamp_us);
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

static void
measure_queues(struct monitor *monitor, uint64_t timestamp_us, struct controller_queue_input *queue)
{
	struct monitor_tcp *tcp = &monitor->tcp;
	struct tcpdelay_capture *capture = &tcp->capture;
	struct tcpdelay_estimate estimate;

	tcpdelay_estimator_result(&capture->estimator, timestamp_us, &estimate);
	if (monitor->config->output_processing_stats) {
		const struct log_tcp_queue_record record = {
			.download_valid = estimate.download_valid,
			.upload_valid = estimate.upload_valid,
			.download_queue_us = estimate.download_queue_us,
			.upload_queue_us = estimate.upload_queue_us,
		};

		log_tcp_queue(&record);
	}
	queue->valid = estimate.download_valid && estimate.upload_valid;
	queue->download_us = estimate.download_queue_us;
	queue->upload_us = estimate.upload_queue_us;
}

/* elapsed is at least TCPDELAY_ACK_RATE_INTERVAL_US, so never zero. */
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
static void
measure_ack_rate(struct monitor *monitor, uint64_t timestamp_us, struct controller_ack_input *acks)
{
	struct monitor_tcp *tcp = &monitor->tcp;
	struct tcpdelay_counters counters;
	uint64_t elapsed_us;

	if (timestamp_us >= tcp->ack_sampled_us + TCPDELAY_ACK_RATE_INTERVAL_US) {
		bool degraded;

		if (tcpdelay_capture_counters(&tcp->capture, &counters) != 0) {
			tcp->ack_rate_valid = false;
			tcp->ack_sampled = false;
			tcp->ack_sampled_us = timestamp_us;
			degraded = true;
			ack_accounting_state(monitor, degraded);
			goto result;
		}
		elapsed_us = timestamp_us - tcp->ack_sampled_us;
		degraded = counters.unaccounted_packets != tcp->unaccounted_packets ||
			   (tcp->ack_sampled && (counters.ack_bytes < tcp->ack_bytes ||
						 counters.upload_bytes < tcp->upload_bytes));
		tcp->ack_rate_valid = false;
		if (tcp->ack_sampled && !degraded) {
			tcp->ack_rate_bps = bps(counters.ack_bytes - tcp->ack_bytes, elapsed_us);
			tcp->upload_rate_bps =
				bps(counters.upload_bytes - tcp->upload_bytes, elapsed_us);
			tcp->ack_rate_valid = true;
		}
		/* A baseline alone cannot establish recovery after unavailable counters. */
		if (degraded || tcp->ack_rate_valid)
			ack_accounting_state(monitor, degraded);
		tcp->ack_bytes = counters.ack_bytes;
		tcp->upload_bytes = counters.upload_bytes;
		tcp->unaccounted_packets = counters.unaccounted_packets;
		tcp->ack_sampled_us = timestamp_us;
		tcp->ack_sampled = true;
	}
result:
	acks->valid = tcp->ack_rate_valid;
	acks->upload_ack_rate_bps = tcp->ack_rate_bps;
	acks->upload_rate_bps = tcp->upload_rate_bps;
}

/*
 * Drains the capture on demand, so the estimates are current whenever the
 * controller runs. Records are submitted without wakeups and wait in the ring
 * buffer until this or the next traffic tick drains them.
 */
void tcp_observe(struct monitor *monitor, struct controller_input *input)
{
	const struct config *config = monitor->config;
	struct monitor_tcp *tcp = &monitor->tcp;
	struct tcpdelay_capture *capture = &tcp->capture;
	struct controller_queue_input *queue = &input->queue;
	struct controller_ack_input *acks = &input->acks;
	int64_t recent_delay_us = reflectors_recent_delay_us(monitor, input->timestamp_us);

	queue->valid = false;
	acks->valid = false;
	if (!capture_ready(monitor))
		return;
	/*
	 * A queue on the access link delays fping's round trips as well, so no TCP
	 * queue may exceed fping's recent added delay. The bound stays until the
	 * next reply, also for records the traffic tick drains.
	 */
	if (recent_delay_us >= 0)
		tcpdelay_estimator_set_bound(&capture->estimator, recent_delay_us);
	/* Records wait in the ring buffer until drained, even without attribution. */
	if (!drain(monitor))
		return;
	report_dropped_records(monitor, input->timestamp_us);
	if (config->tcp_delay_attribution)
		measure_queues(monitor, input->timestamp_us, queue);
	if (config->ul_congest_ack_share_ratio_e6 != 0U)
		measure_ack_rate(monitor, input->timestamp_us, acks);
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
	detach_injector(monitor);
	tcpdelay_capture_close(&tcp->capture);
	tcp->open = false;
	tcp->ack_sampled = false;
	tcp->ack_rate_valid = false;
}

void tcp_stop(struct monitor *monitor)
{
	tcp_close(monitor);
	tcpdelay_injector_unload(&monitor->tcp.injector);
	tcpdelay_capture_unload(&monitor->tcp.capture);
}
