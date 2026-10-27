#define _GNU_SOURCE

#include "daemon/state.h"

#include "common/error.h"
#include "common/rate.h"
#include "config/defaults.h"
#include "logging/log.h"
#include "logging/records.h"
#include "constants/tcpdelay.h"
#include "constants/text.h"

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

/* The upload ACK hold, which needs the filter's byte counters. */
static bool ack_share_enabled(const struct config *config)
{
	return config->ul_congest_ack_share_ratio_e6 != 0U;
}

/* Either TCP feature needs the capture. */
static bool tcp_enabled(const struct config *config)
{
	return config->tcp_delay_attribution || ack_share_enabled(config);
}

void tcp_init(struct daemon *daemon)
{
	struct daemon_tcp *tcp = &daemon->tcp;

	tcpdelay_capture_init(&tcp->capture);
	tcpdelay_injector_init(&tcp->injector);
}

void tcp_start(struct daemon *daemon)
{
	const struct config *config = daemon->config;
	struct daemon_tcp *tcp = &daemon->tcp;
	char error[ERROR_SIZE] = { 0 };

	/* The injector shares the filter's object; validation requires attribution. */
	if (config->tcp_ts_request)
		tcpdelay_capture_enable_injection(&tcp->capture);
	if (tcp_enabled(config) && tcpdelay_capture_load(&tcp->capture, error, sizeof(error)) != 0)
		log_message(
			LOG_LEVEL_WARNING,
			"Warning: TCP measurement unavailable: %s. Continuing without it.",
			error
		);
}

static struct log_tcp_inject_record inject_record(const struct tcpdelay_inject_counters *counters)
{
	return (struct log_tcp_inject_record){
		.injected = counters->injected,
		.skipped = counters->skipped,
		.accepted = counters->accepted,
		.stalled = counters->stalled,
	};
}

/* Experimental: follows the capture onto the same interface. */
static void attach_injector(struct daemon *daemon, const char *interface)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	char error[ERROR_SIZE] = { 0 };

	if (!daemon->config->tcp_ts_request)
		return;
	if (tcpdelay_injector_attach(&tcp->injector, &tcp->capture, interface, error, sizeof(error)) !=
	    0) {
		log_message(
			LOG_LEVEL_WARNING,
			"Warning: TCP timestamp injection unavailable: %s. Continuing without it.",
			error
		);
		return;
	}
	log_message(
		LOG_LEVEL_NOTICE,
		"Starting TCP timestamp injection on interface: %s",
		interface
	);
}

static void detach_injector(struct daemon *daemon)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	struct tcpdelay_injector *injector = &tcp->injector;
	struct tcpdelay_inject_counters counters;

	if (!tcpdelay_injector_attached(injector))
		return;
	if (tcpdelay_injector_counters(injector, &counters) == 0)
		log_message(
			LOG_LEVEL_NOTICE,
			"Stopped TCP timestamp injection with injected SYNs: %" PRIu64
			" and stalls: %" PRIu64,
			(uint64_t)counters.injected,
			(uint64_t)counters.stalled
		);
	tcpdelay_injector_detach(injector);
}

static bool open_capture(struct daemon *daemon, const struct links_direction *upload)
{
	const struct cake_observation *cake = &upload->cake;
	const struct netlink_qdisc_id *qdisc = &cake->qdisc;
	struct daemon_tcp *tcp = &daemon->tcp;
	char error[ERROR_SIZE] = { 0 };
	const struct cake_accounting model = accounting_model(cake);
	const struct cake_accounting *accounting = ack_share_enabled(daemon->config) ? &model :
										       NULL;

	if (tcpdelay_capture_open(
		    &tcp->capture,
		    upload->interface,
		    accounting,
		    error,
		    sizeof(error)
	    ) != 0) {
		log_message(
			LOG_LEVEL_WARNING,
			"Warning: TCP measurement unavailable: %s. Continuing without it.",
			error
		);
		tcp->failed_index = qdisc->interface_index;
		return false;
	}
	tcp->open = true;
	tcp->qdisc = *qdisc;
	tcp->failed_index = 0U;
	tcp->dropped_records = 0U;
	tcp->next_counter_check_us = 0U;
	/* The counters restart with the new filter. */
	tcp->ack_sampled = false;
	tcp->ack_rate_valid = false;
	tcp->ack_degraded = false;
	tcp->unaccounted_packets = 0U;
	log_message(
		LOG_LEVEL_NOTICE,
		"Starting TCP measurement on interface: %s",
		upload->interface
	);
	attach_injector(daemon, upload->interface);
	return true;
}

/* Once a minute: the injector's counters. */
static void log_injector(struct daemon *daemon)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	struct tcpdelay_inject_counters counters;
	struct log_tcp_inject_record record;

	if (!daemon->config->output_processing_stats ||
	    tcpdelay_injector_counters(&tcp->injector, &counters) != 0)
		return;
	record = inject_record(&counters);
	log_tcp_inject(&record);
}

static void report_dropped_records(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	struct tcpdelay_counters counters;

	if (timestamp_us < tcp->next_counter_check_us)
		return;
	tcp->next_counter_check_us = timestamp_us + TCPDELAY_COUNTER_CHECK_US;
	if (tcpdelay_injector_attached(&tcp->injector))
		log_injector(daemon);
	if (tcpdelay_capture_counters(&tcp->capture, &counters) != 0)
		return;
	if (counters.ring_full > tcp->dropped_records) {
		log_message(
			LOG_LEVEL_WARNING,
			"Warning: TCP delay records dropped since last check: %" PRIu64,
			(uint64_t)(counters.ring_full - tcp->dropped_records)
		);
		tcp->dropped_records = counters.ring_full;
	}
}

/* Adds pending records to the estimator; false when the capture failed and was closed. */
static bool drain(struct daemon *daemon)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	const struct netlink_qdisc_id *qdisc = &tcp->qdisc;

	if (tcpdelay_capture_drain(&tcp->capture) >= 0)
		return true;
	log_message(
		LOG_LEVEL_WARNING,
		"Warning: TCP measurement capture failed: %s. Continuing without it.",
		strerror(errno)
	);
	tcp_close(daemon);
	/* Not retried until the upload interface the capture was bound to is recreated. */
	tcp->failed_index = qdisc->interface_index;
	return false;
}

/* The open capture was set up for another upload qdisc or CAKE accounting. */
static bool capture_outdated(const struct daemon_tcp *tcp, const struct cake_observation *cake)
{
	const struct tcpdelay_capture *capture = &tcp->capture;
	const struct tcpdelay_accounting *accounting = &capture->accounting;
	const struct cake_accounting model = accounting_model(cake);

	return !qdisc_same(&tcp->qdisc, &cake->qdisc) ||
	       (accounting->enabled && memcmp(&accounting->cake, &model, sizeof(model)) != 0);
}

/* Opens the capture, or reopens it for a changed qdisc; false when it is not usable. */
static bool capture_ready(struct daemon *daemon)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	const struct config *config = daemon->config;
	const struct daemon_links *links = &daemon->links;
	const struct links_direction *upload = &links->directions[DIRECTION_UPLOAD];
	const struct cake_observation *cake = &upload->cake;
	const struct netlink_qdisc_id *qdisc = &cake->qdisc;

	if (!tcp_enabled(config) || upload->cake_state != LINKS_CAKE_AVAILABLE) {
		tcp_close(daemon);
		return false;
	}
	if (tcp->open && capture_outdated(tcp, cake)) {
		log_message(
			LOG_LEVEL_INFO,
			"Reopening TCP capture for changed interface or CAKE accounting on interface: %s",
			upload->interface
		);
		tcp_close(daemon);
	}
	if (tcp->open)
		return true;
	return tcp->failed_index != qdisc->interface_index && open_capture(daemon, upload);
}

static void
measure_queues(struct daemon *daemon, uint64_t timestamp_us, struct controller_queue_input *queue)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	struct tcpdelay_capture *capture = &tcp->capture;
	const struct config *config = daemon->config;
	struct tcpdelay_estimate estimate;
	struct log_tcp_queue_record record;

	tcpdelay_estimator_result(&capture->estimator, timestamp_us, &estimate);
	queue->valid = true;
	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
		record.valid[direction] = estimate.valid[direction];
		record.queue_us[direction] = estimate.queue_us[direction];
		queue->valid = queue->valid && estimate.valid[direction];
		queue->delay_us[direction] = estimate.queue_us[direction];
	}
	if (config->output_processing_stats)
		log_tcp_queue(&record);
}

bool tcp_estimate(struct daemon *daemon, uint64_t timestamp_us, struct tcpdelay_estimate *estimate)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	struct tcpdelay_capture *capture = &tcp->capture;

	if (!tcp->open)
		return false;
	tcpdelay_estimator_result(&capture->estimator, timestamp_us, estimate);
	return true;
}

static void ack_accounting_state(struct daemon *daemon, bool degraded)
{
	struct daemon_tcp *tcp = &daemon->tcp;

	if (tcp->ack_degraded == degraded)
		return;
	tcp->ack_degraded = degraded;
	if (degraded)
		log_message(
			LOG_LEVEL_WARNING,
			"Warning: incomplete CAKE accounting or unavailable counters. Disabling ACK ceiling."
		);
	else
		log_message(LOG_LEVEL_NOTICE, "ACK accounting recovered. Resuming ACK ceiling.");
}

/*
 * Pure-ACK and total upload rates from the filter's byte counters, over >= 500 ms;
 * elapsed is at least TCPDELAY_ACK_RATE_INTERVAL_US, so never zero.
 */
static void
measure_ack_rate(struct daemon *daemon, uint64_t timestamp_us, struct controller_ack_input *acks)
{
	struct daemon_tcp *tcp = &daemon->tcp;
	struct tcpdelay_counters counters;
	uint64_t elapsed_us;

	if (timestamp_us >= tcp->ack_sampled_us + TCPDELAY_ACK_RATE_INTERVAL_US) {
		bool degraded;

		if (tcpdelay_capture_counters(&tcp->capture, &counters) != 0) {
			tcp->ack_rate_valid = false;
			tcp->ack_sampled = false;
			tcp->ack_sampled_us = timestamp_us;
			degraded = true;
			ack_accounting_state(daemon, degraded);
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
			ack_accounting_state(daemon, degraded);
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
void tcp_observe(struct daemon *daemon, struct controller_input *input)
{
	const struct config *config = daemon->config;
	struct daemon_tcp *tcp = &daemon->tcp;
	struct tcpdelay_capture *capture = &tcp->capture;
	struct controller_queue_input *queue = &input->queue;
	struct controller_ack_input *acks = &input->acks;
	int64_t recent_delay_us = reflectors_recent_delay_us(daemon, input->timestamp_us);

	queue->valid = false;
	acks->valid = false;
	if (!capture_ready(daemon))
		return;
	/*
	 * A queue on the access link delays the echo pinger's round trips as well,
	 * so no TCP queue may exceed their recent added delay. The bound stays until the
	 * next reply, also for records the traffic tick drains.
	 */
	if (recent_delay_us >= 0)
		tcpdelay_estimator_set_bound(&capture->estimator, recent_delay_us);
	/* Records wait in the ring buffer until drained, even without attribution. */
	if (!drain(daemon))
		return;
	report_dropped_records(daemon, input->timestamp_us);
	if (config->tcp_delay_attribution)
		measure_queues(daemon, input->timestamp_us, queue);
	if (ack_share_enabled(config))
		measure_ack_rate(daemon, input->timestamp_us, acks);
}

/*
 * The controller drains the ring on every ping reply; the traffic tick drains
 * it too, so records do not overflow while no reply runs the controller, as
 * when pingers are stopped in IDLE or while CAKE is missing.
 */
void tcp_drain(struct daemon *daemon)
{
	struct daemon_links *links = &daemon->links;
	struct links_direction *upload = &links->directions[DIRECTION_UPLOAD];
	struct daemon_tcp *tcp = &daemon->tcp;

	if (upload->cake_state != LINKS_CAKE_AVAILABLE) {
		tcp_close(daemon);
		return;
	}
	if (tcp->open)
		(void)drain(daemon);
}

void tcp_close(struct daemon *daemon)
{
	struct daemon_tcp *tcp = &daemon->tcp;

	if (!tcp->open)
		return;
	detach_injector(daemon);
	tcpdelay_capture_close(&tcp->capture);
	tcp->open = false;
	tcp->ack_sampled = false;
	tcp->ack_rate_valid = false;
}

void tcp_stop(struct daemon *daemon)
{
	struct daemon_tcp *tcp = &daemon->tcp;

	tcp_close(daemon);
	tcpdelay_injector_unload(&tcp->injector);
	tcpdelay_capture_unload(&tcp->capture);
}
