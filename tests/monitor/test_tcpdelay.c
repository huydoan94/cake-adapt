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

void log_tcp_inject(const struct log_tcp_inject_record *record)
{
	(void)record;
}

static unsigned int opened;
static unsigned int closed;

int tcpdelay_capture_open(
	struct tcpdelay_capture *capture,
	const char *interface,
	const struct cake_accounting *accounting,
	char *error,
	size_t error_size
)
{
	(void)interface;
	(void)error;
	(void)error_size;
	opened++;
	capture->interface_index = 10U;
	capture->accounting.enabled = accounting != NULL;
	if (accounting != NULL)
		capture->accounting.cake = *accounting;
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

void tcpdelay_estimator_set_bound(struct tcpdelay_estimator *estimator, int64_t queue_bound_us)
{
	(void)estimator;
	(void)queue_bound_us;
}

static unsigned int injector_attaches;
static unsigned int injector_detaches;
static int injector_status;
static bool injector_on;
static struct tcpdelay_inject_state injected_state;
static unsigned int state_writes;

int tcpdelay_injector_attach(
	struct tcpdelay_injector *injector,
	const struct tcpdelay_capture *capture,
	const char *interface,
	char *error,
	size_t error_size
)
{
	(void)injector;
	(void)capture;
	(void)error;
	(void)error_size;
	assert(strcmp(interface, "wan") == 0);
	injector_attaches++;
	injector_on = injector_status == 0;
	return injector_status;
}

bool tcpdelay_injector_attached(const struct tcpdelay_injector *injector)
{
	(void)injector;
	return injector_on;
}

int tcpdelay_injector_counters(
	const struct tcpdelay_injector *injector,
	struct tcpdelay_inject_counters *counters
)
{
	(void)injector;
	*counters = (struct tcpdelay_inject_counters){
		.injected = 3U,
		.server_accepted = 2U,
	};
	return 0;
}

int tcpdelay_injector_state(
	const struct tcpdelay_capture *capture,
	struct tcpdelay_inject_state *state
)
{
	(void)capture;
	*state = injected_state;
	return 0;
}

int tcpdelay_injector_set_state(
	const struct tcpdelay_capture *capture,
	const struct tcpdelay_inject_state *state
)
{
	(void)capture;
	injected_state = *state;
	state_writes++;
	return 0;
}

void tcpdelay_injector_detach(struct tcpdelay_injector *injector)
{
	(void)injector;
	injector_detaches++;
	injector_on = false;
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
	struct config config = { .ul_congest_ack_share_ratio_e6 = 450000U };
	struct monitor_direction *upload = &monitor->links.upload;
	const struct cake_observation cake = {
		.qdisc = { .interface_index = 10U, .handle = 0x10000U, .parent = TC_H_ROOT },
		.atm_mode = CAKE_ATM_NONE,
	};
	const struct qdisc_event removed = {
		.type = QDISC_REMOVED,
		.qdisc = cake.qdisc,
	};
	monitor->config = &config;
	upload->cake = cake;
	upload->cake_state = CAKE_OBSERVATION_AVAILABLE;
	assert(capture_ready(monitor));
	assert(opened == 1U && closed == 0U);
	monitor->tcp.ack_sampled = true;
	monitor->tcp.ack_rate_valid = true;
	/* Removal is observed immediately, even if recreation keeps every key. */
	process_qdisc_event(&monitor->netlink, &removed);
	assert(closed == 1U && !monitor->tcp.open);
	assert(!monitor->tcp.ack_sampled && !monitor->tcp.ack_rate_valid);
	upload->cake = cake;
	upload->cake_state = CAKE_OBSERVATION_AVAILABLE;
	assert(capture_ready(monitor));
	assert(opened == 2U && !monitor->tcp.ack_sampled);
	upload->cake.overhead_bytes = 44;
	assert(capture_ready(monitor));
	assert(opened == 3U && closed == 2U);
	assert(monitor->tcp.capture.accounting.cake.overhead_bytes == 44);
	upload->cake.mpu_bytes = 84U;
	assert(capture_ready(monitor));
	assert(opened == 4U && closed == 3U);
	assert(monitor->tcp.capture.accounting.cake.mpu_bytes == 84U);
	/* Discovery failure also closes the capture from the traffic tick. */
	upload->cake_state = CAKE_OBSERVATION_FAILED;
	tcp_drain(monitor);
	assert(closed == 4U && !monitor->tcp.open);
	monitor->config = NULL;
}

/* The injector follows the capture onto the upload interface, and never blocks it. */
static void injector_lifecycle(struct monitor *monitor)
{
	struct config config = {
		.tcp_delay_attribution = true,
		.tcp_timestamp_inject = true,
	};
	struct monitor_direction *upload = &monitor->links.upload;
	unsigned int warned = warnings;

	monitor->config = &config;
	upload->interface = "wan";
	upload->cake.qdisc = (struct qdisc_id){ .interface_index = 10U, .handle = 0x10000U };
	upload->cake_state = CAKE_OBSERVATION_AVAILABLE;
	assert(capture_ready(monitor));
	assert(injector_attaches == 1U && injector_on);
	tcp_close(monitor);
	assert(injector_detaches == 1U && !injector_on);
	/* A failed attachment warns; the capture stays open and nothing is detached. */
	injector_status = -1;
	assert(capture_ready(monitor));
	assert(monitor->tcp.open && injector_attaches == 2U && warnings == warned + 1U);
	tcp_close(monitor);
	assert(injector_detaches == 1U);
	/* Off by default: no attachment at all. */
	injector_status = 0;
	config.tcp_timestamp_inject = false;
	assert(capture_ready(monitor));
	assert(injector_attaches == 2U);
	tcp_close(monitor);
	monitor->config = NULL;
}

/* A recorded stall burst is resolved within a second and written back, once. */
static void injector_resolve(struct monitor *monitor)
{
	struct config config = {
		.tcp_delay_attribution = true,
		.tcp_timestamp_inject = true,
	};
	struct monitor_direction *upload = &monitor->links.upload;
	unsigned int warned = warnings;

	monitor->config = &config;
	upload->interface = "wan";
	upload->cake.qdisc = (struct qdisc_id){ .interface_index = 10U, .handle = 0x10000U };
	upload->cake_state = CAKE_OBSERVATION_AVAILABLE;
	assert(capture_ready(monitor) && injector_on);
	resolve_injection(monitor, 10U * SECOND);
	assert(warnings == warned && state_writes == 0U);
	/* A burst with nothing learned pauses, logged and written back once. */
	injected_state.burst = 1U;
	resolve_injection(monitor, 11U * SECOND);
	assert(warnings == warned + 1U && state_writes == 1U);
	assert(injected_state.mode == TCPDELAY_INJECT_PAUSED && !injected_state.burst);
	/* Not again within the second. */
	injected_state.burst = 1U;
	resolve_injection(monitor, 11U * SECOND + 500U * MILLISECOND);
	assert(state_writes == 1U);
	injected_state.burst = 0U;
	/* The program stays attached; IPv6 keeps injecting. */
	assert(injector_on);
	tcp_close(monitor);
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
	assert(acks.upload_ack_rate_bps == 8000U);
	assert(acks.upload_rate_bps == 16000U);

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
	assert(acks.upload_ack_rate_bps == 8000U);

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
	assert(bps(1U, 3U * SECOND) == 2U);
	capture_lifecycle(monitor);
	injector_lifecycle(monitor);
	injector_resolve(monitor);
	free(monitor);
	puts("monitor ACK accounting tests passed");
	return 0;
}
