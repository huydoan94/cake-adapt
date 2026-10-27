/* Exercise the daemon's actual ACK sampling with deterministic capture reads. */
#include "daemon/tcp.c"
#include "daemon/links.c"
#include "constants/units.h"

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
	struct tcpdelay_accounting *settings = &capture->accounting;

	(void)interface;
	(void)error;
	(void)error_size;
	opened++;
	capture->interface_index = 10U;
	settings->enabled = accounting != NULL;
	if (accounting != NULL)
		settings->cake = *accounting;
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
		.skipped = 1U,
		.accepted = 2U,
	};
	return 0;
}

void tcpdelay_injector_detach(struct tcpdelay_injector *injector)
{
	(void)injector;
	injector_detaches++;
	injector_on = false;
}

void traffic_init(struct traffic_monitor *daemon)
{
	memset(daemon, 0, sizeof(*daemon));
}

void pingers_close(struct daemon *daemon)
{
	(void)daemon;
}

static void capture_lifecycle(struct daemon *daemon)
{
	struct config config = { .ul_congest_ack_share_ratio_e6 = 450000U };
	struct links_direction *upload = &daemon->links.directions[DIRECTION_UPLOAD];
	struct cake_observation *upload_cake = &upload->cake;
	struct daemon_tcp *tcp = &daemon->tcp;
	const struct cake_accounting *filter_accounting = &tcp->capture.accounting.cake;
	const struct cake_observation cake = {
		.qdisc = { .interface_index = 10U, .handle = 0x10000U, .parent = TC_H_ROOT },
		.atm_mode = CAKE_ATM_NONE,
	};
	const struct netlink_qdisc_event removed = {
		.type = NETLINK_QDISC_EVENT_REMOVED,
		.qdisc = cake.qdisc,
	};
	daemon->config = &config;
	upload->cake = cake;
	upload->cake_state = LINKS_CAKE_AVAILABLE;
	assert(capture_ready(daemon));
	assert(opened == 1U && closed == 0U);
	tcp->ack_sampled = true;
	tcp->ack_rate_valid = true;
	/* Removal is observed immediately, even if recreation keeps every key. */
	process_qdisc_event(&daemon->netlink, &removed);
	assert(closed == 1U && !tcp->open);
	assert(!tcp->ack_sampled && !tcp->ack_rate_valid);
	upload->cake = cake;
	upload->cake_state = LINKS_CAKE_AVAILABLE;
	assert(capture_ready(daemon));
	assert(opened == 2U && !tcp->ack_sampled);
	upload_cake->overhead_bytes = 44;
	assert(capture_ready(daemon));
	assert(opened == 3U && closed == 2U);
	assert(filter_accounting->overhead_bytes == 44);
	upload_cake->mpu_bytes = 84U;
	assert(capture_ready(daemon));
	assert(opened == 4U && closed == 3U);
	assert(filter_accounting->mpu_bytes == 84U);
	/* Discovery failure also closes the capture from the traffic tick. */
	upload->cake_state = LINKS_CAKE_FAILED;
	tcp_drain(daemon);
	assert(closed == 4U && !tcp->open);
	daemon->config = NULL;
}

/* The injector follows the capture onto the upload interface, and never blocks it. */
static void injector_lifecycle(struct daemon *daemon)
{
	struct config config = {
		.tcp_delay_attribution = true,
		.tcp_ts_request = true,
	};
	struct links_direction *upload = &daemon->links.directions[DIRECTION_UPLOAD];
	struct cake_observation *upload_cake = &upload->cake;
	const struct daemon_tcp *tcp = &daemon->tcp;
	unsigned int warned = warnings;

	daemon->config = &config;
	upload->interface = "wan";
	upload_cake->qdisc =
		(struct netlink_qdisc_id){ .interface_index = 10U, .handle = 0x10000U };
	upload->cake_state = LINKS_CAKE_AVAILABLE;
	assert(capture_ready(daemon));
	assert(injector_attaches == 1U && injector_on);
	tcp_close(daemon);
	assert(injector_detaches == 1U && !injector_on);
	/* A failed attachment warns; the capture stays open and nothing is detached. */
	injector_status = -1;
	assert(capture_ready(daemon));
	assert(tcp->open && injector_attaches == 2U && warnings == warned + 1U);
	tcp_close(daemon);
	assert(injector_detaches == 1U);
	/* Off by default: no attachment at all. */
	injector_status = 0;
	config.tcp_ts_request = false;
	assert(capture_ready(daemon));
	assert(injector_attaches == 2U);
	tcp_close(daemon);
	daemon->config = NULL;
}

/* A failing observation warns once; only its recovery logs again, once. */
static void test_failure_and_recovery_log_once(void)
{
	unsigned int warned = warnings;
	unsigned int noticed = notices;
	bool failed = false;

	daemon_recovered(&failed, "recovered before any failure");
	assert(notices == noticed);
	daemon_degraded(&failed, "degraded: %s", "first");
	daemon_degraded(&failed, "degraded: %s", "again");
	assert(failed && warnings == warned + 1U);
	daemon_recovered(&failed, "recovered");
	daemon_recovered(&failed, "recovered again");
	assert(!failed && notices == noticed + 1U);
	daemon_degraded(&failed, "degraded: %s", "later");
	assert(warnings == warned + 2U);
}

int main(void)
{
	struct daemon *daemon = calloc(1U, sizeof(*daemon));
	struct controller_ack_input acks = { 0 };
	const struct daemon_tcp *tcp;

	assert(daemon != NULL);
	tcp = &daemon->tcp;
	supplied.ack_bytes = 1000U;
	supplied.upload_bytes = 2000U;
	measure_ack_rate(daemon, 500U * MILLISECOND, &acks);
	assert(!acks.valid);
	supplied.ack_bytes += 500U;
	supplied.upload_bytes += 1000U;
	measure_ack_rate(daemon, SECOND, &acks);
	assert(acks.valid);
	assert(acks.upload_ack_rate_bps == 8000U);
	assert(acks.upload_rate_bps == 16000U);

	supplied.unaccounted_packets++;
	measure_ack_rate(daemon, 1500U * MILLISECOND, &acks);
	assert(!acks.valid);
	assert(warnings == 1U);
	/* Repeated bad intervals do not flood syslog. */
	supplied.unaccounted_packets++;
	measure_ack_rate(daemon, 2U * SECOND, &acks);
	assert(!acks.valid && warnings == 1U);
	supplied.ack_bytes += 500U;
	supplied.upload_bytes += 1000U;
	measure_ack_rate(daemon, 2500U * MILLISECOND, &acks);
	assert(acks.valid && notices == 1U);
	assert(acks.upload_ack_rate_bps == 8000U);

	/* Reset all counters, then establish a clean rate from the new baseline. */
	supplied = (struct tcpdelay_counters){ 0 };
	measure_ack_rate(daemon, 3U * SECOND, &acks);
	assert(!acks.valid && warnings == 2U);
	supplied.ack_bytes = 500U;
	supplied.upload_bytes = 1000U;
	measure_ack_rate(daemon, 3500U * MILLISECOND, &acks);
	assert(acks.valid && notices == 2U);

	/* A failed read discards the baseline; recovery needs two successful reads. */
	read_status = -1;
	measure_ack_rate(daemon, 4U * SECOND, &acks);
	assert(!acks.valid && warnings == 3U);
	read_status = 0;
	measure_ack_rate(daemon, 4500U * MILLISECOND, &acks);
	assert(!acks.valid && tcp->ack_degraded);
	assert(notices == 2U);
	supplied.ack_bytes += 500U;
	supplied.upload_bytes += 1000U;
	measure_ack_rate(daemon, 5U * SECOND, &acks);
	assert(acks.valid && notices == 3U);
	assert(bps(1U, 3U * SECOND) == 2U);
	capture_lifecycle(daemon);
	injector_lifecycle(daemon);
	free(daemon);
	test_failure_and_recovery_log_once();
	puts("daemon ACK accounting tests passed");
	return 0;
}
