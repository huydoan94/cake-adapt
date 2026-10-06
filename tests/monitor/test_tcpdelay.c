/* Exercise the monitor's actual ACK sampling with deterministic capture reads. */
#include "monitor/tcpdelay.c"
#include "monitor/links.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static struct tcpdelay_counters supplied;
static int read_status;
static unsigned int warnings;
static unsigned int notices;

int tcpdelay_capture_counters(
	const struct tcpdelay_capture *capture,
	struct tcpdelay_counters *counters
)
{
	(void)capture;
	*counters = supplied;
	return read_status;
}

void log_message(enum log_level level, const char *format, ...)
{
	(void)format;
	if (level == LOG_LEVEL_WARNING)
		warnings++;
	if (level == LOG_LEVEL_NOTICE)
		notices++;
}

void log_tcp_queue(const struct log_tcp_queue_record *record)
{
	(void)record;
}

static unsigned int opened;
static unsigned int closed;

int tcpdelay_capture_open(
	struct tcpdelay_capture *capture,
	const char *object_path,
	const char *interface,
	const struct cake_accounting *accounting,
	struct tcpdelay_estimator *estimator,
	char *error,
	size_t error_size
)
{
	(void)object_path;
	(void)interface;
	(void)estimator;
	(void)error;
	(void)error_size;
	opened++;
	capture->interface_index = 10U;
	capture->state = TCPDELAY_CAPTURE_READY;
	capture->accounting.cake = *accounting;
	capture->accounting.enabled = 1U;
	return 0;
}

void tcpdelay_capture_close(struct tcpdelay_capture *capture)
{
	(void)capture;
	closed++;
}

int tcpdelay_capture_drain(struct tcpdelay_capture *capture)
{
	(void)capture;
	return 0;
}

bool tcpdelay_capture_timing_available(const struct tcpdelay_capture *capture)
{
	return capture->state == TCPDELAY_CAPTURE_READY && !capture->ring_busy;
}

void tcpdelay_estimator_init(struct tcpdelay_estimator *estimator)
{
	(void)estimator;
}

void tcpdelay_estimator_set_policy(
	struct tcpdelay_estimator *estimator,
	enum tcpdelay_baseline_policy policy
)
{
	(void)estimator;
	(void)policy;
}

static unsigned int estimator_results;

void tcpdelay_estimator_result(
	const struct tcpdelay_estimator *estimator,
	uint64_t now_ns,
	struct tcpdelay_estimate *estimate
)
{
	(void)estimator;
	(void)now_ns;
	estimator_results++;
	*estimate = (struct tcpdelay_estimate){ .download_valid = true, .upload_valid = true };
}

void traffic_init(struct traffic_monitor *monitor)
{
	memset(monitor, 0, sizeof(*monitor));
}

void pingers_close(struct monitor *monitor)
{
	(void)monitor;
}

static void capture_lifecycle(struct monitor *monitor)
{
	struct config config = { .ul_congest_ack_share_per_million = 450000U };
	struct monitor_direction *upload = &monitor->links.upload;
	const struct cake_observation cake = {
		.interface_index = 10U,
		.handle = 0x10000U,
		.parent = TC_H_ROOT,
		.atm_mode = CAKE_ATM_NONE,
	};
	const struct qdisc_event removed = {
		.type = QDISC_REMOVED,
		.interface_index = 10U,
		.handle = 0x10000U,
		.parent = TC_H_ROOT,
	};
	monitor->config = &config;
	upload->cake = cake;
	upload->cake_state = CAKE_OBSERVATION_AVAILABLE;
	assert(capture_ready(monitor, 6U * SECOND, TCPDELAY_BASELINE_HOLD));
	assert(opened == 1U && closed == 0U);
	monitor->tcp.ack_sampled = true;
	monitor->tcp.ack_rate_valid = true;
	/* Removal is observed immediately, even if recreation keeps every key. */
	process_qdisc_event(&removed, monitor);
	assert(closed == 1U && !monitor->tcp.open);
	assert(!monitor->tcp.ack_sampled && !monitor->tcp.ack_rate_valid);
	upload->cake = cake;
	upload->cake_state = CAKE_OBSERVATION_AVAILABLE;
	assert(capture_ready(monitor, 7U * SECOND, TCPDELAY_BASELINE_HOLD));
	assert(opened == 2U && !monitor->tcp.ack_sampled);
	upload->cake.overhead_bytes = 44;
	assert(capture_ready(monitor, 8U * SECOND, TCPDELAY_BASELINE_HOLD));
	assert(opened == 3U && closed == 2U);
	assert(monitor->tcp.capture.accounting.cake.overhead_bytes == 44);
	upload->cake.mpu_bytes = 84U;
	assert(capture_ready(monitor, 9U * SECOND, TCPDELAY_BASELINE_HOLD));
	assert(opened == 4U && closed == 3U);
	assert(monitor->tcp.capture.accounting.cake.mpu_bytes == 84U);
	/* Discovery failure also closes the capture from the traffic tick. */
	upload->cake_state = CAKE_OBSERVATION_FAILED;
	tcp_drain(monitor);
	assert(closed == 4U && !monitor->tcp.open);
	monitor->config = NULL;
}

static void test_queue_confidence_follows_capture_state(struct monitor *monitor)
{
	struct config config = {
		.tcp_delay_attribution = true,
		.ul_congest_ack_share_per_million = 450000U,
	};
	struct controller_input input = { 0 };
	struct monitor_direction *upload = &monitor->links.upload;
	const struct cake_observation cake = {
		.interface_index = 10U,
		.handle = 0x10000U,
		.parent = TC_H_ROOT,
		.atm_mode = CAKE_ATM_NONE,
	};

	monitor->config = &config;
	upload->cake = cake;
	upload->cake_state = CAKE_OBSERVATION_AVAILABLE;
	supplied = (struct tcpdelay_counters){
		.ack_bytes = 10000U,
		.upload_bytes = 20000U,
	};
	input.timestamp_microseconds = 10U * SECOND;
	tcp_observe(monitor, &input);
	assert(estimator_results == 1U);
	assert(!input.acks.valid);
	{
		unsigned int warning_count = warnings;

		monitor->tcp.capture.ring_busy = true;
		tcp_observe(monitor, &input);
		assert(estimator_results == 1U && warnings == warning_count);
		assert(monitor->tcp.queue_timing_available);
		monitor->tcp.capture.ring_busy = false;
	}
	monitor->tcp.capture.state = TCPDELAY_CAPTURE_RECOVERING;
	supplied.ack_bytes += 500U;
	supplied.upload_bytes += 1000U;
	input.timestamp_microseconds = 10500U * MILLISECOND;
	tcp_observe(monitor, &input);
	assert(estimator_results == 1U);
	assert(!input.queue.valid);
	assert(input.acks.valid);
	{
		unsigned int notice_count = notices;

		monitor->tcp.capture.state = TCPDELAY_CAPTURE_READY;
		monitor->tcp.capture.ring_busy = true;
		tcp_observe(monitor, &input);
		assert(estimator_results == 1U && notices == notice_count);
		monitor->tcp.capture.ring_busy = false;
		tcp_observe(monitor, &input);
		assert(estimator_results == 2U && notices == notice_count + 1U);
	}
	monitor->tcp.capture.state = TCPDELAY_CAPTURE_DISABLED;
	supplied.ack_bytes += 500U;
	supplied.upload_bytes += 1000U;
	input.timestamp_microseconds = 11U * SECOND;
	tcp_observe(monitor, &input);
	assert(estimator_results == 2U);
	assert(input.acks.valid);
	monitor->config = NULL;
}

int main(void)
{
	struct monitor *monitor = calloc(1U, sizeof(*monitor));
	struct controller_ack_input acks = { 0 };

	assert(monitor != NULL);
	supplied.ack_bytes = 1000U;
	supplied.upload_bytes = 2000U;
	measure_ack_rate(monitor, 500U * MILLISECOND, &acks);
	assert(!acks.valid);
	supplied.ack_bytes += 500U;
	supplied.upload_bytes += 1000U;
	measure_ack_rate(monitor, SECOND, &acks);
	assert(acks.valid);
	assert(acks.upload_ack_rate_bits_per_second == 8000U);
	assert(acks.upload_rate_bits_per_second == 16000U);

	supplied.unaccounted_packets++;
	measure_ack_rate(monitor, 1500U * MILLISECOND, &acks);
	assert(!acks.valid);
	assert(warnings == 1U);
	/* Repeated bad intervals do not flood syslog. */
	supplied.unaccounted_packets++;
	measure_ack_rate(monitor, 2U * SECOND, &acks);
	assert(!acks.valid && warnings == 1U);
	supplied.ack_bytes += 500U;
	supplied.upload_bytes += 1000U;
	measure_ack_rate(monitor, 2500U * MILLISECOND, &acks);
	assert(acks.valid && notices == 1U);
	assert(acks.upload_ack_rate_bits_per_second == 8000U);

	/* Reset all counters, then establish a clean rate from the new baseline. */
	supplied = (struct tcpdelay_counters){ 0 };
	measure_ack_rate(monitor, 3U * SECOND, &acks);
	assert(!acks.valid && warnings == 2U);
	supplied.ack_bytes = 500U;
	supplied.upload_bytes = 1000U;
	measure_ack_rate(monitor, 3500U * MILLISECOND, &acks);
	assert(acks.valid && notices == 2U);

	/* A failed read discards the baseline; recovery needs two successful reads. */
	read_status = -1;
	measure_ack_rate(monitor, 4U * SECOND, &acks);
	assert(!acks.valid && warnings == 3U);
	read_status = 0;
	measure_ack_rate(monitor, 4500U * MILLISECOND, &acks);
	assert(!acks.valid && monitor->tcp.ack_degraded);
	assert(notices == 2U);
	supplied.ack_bytes += 500U;
	supplied.upload_bytes += 1000U;
	measure_ack_rate(monitor, 5U * SECOND, &acks);
	assert(acks.valid && notices == 3U);
	assert(rate_since(1U, 0U, 3U * SECOND) == 2U);
	capture_lifecycle(monitor);
	test_queue_confidence_follows_capture_state(monitor);
	free(monitor);
	puts("monitor ACK accounting tests passed");
	return 0;
}
