/*
 * report_decision() from synthetic decisions: which messages a changed
 * bufferbloat attribution and a changed congestion state produce, with and
 * without tcp_delay_attribution.
 */
#include "daemon/report.c"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Every message report_decision() logged, and its level, in order. */
static char messages[8][256];
static enum log_level levels[8];
static size_t message_count;

void log_message(enum log_level level, const char *format, ...)
{
	va_list arguments;

	assert(message_count < ARRAY_SIZE(messages));
	levels[message_count] = level;
	va_start(arguments, format);
	(void)vsnprintf(messages[message_count], sizeof(messages[0]), format, arguments);
	va_end(arguments);
	message_count++;
}

/* report_records() is not under test; these keep it linkable. */
void log_data(const struct log_data_record *record)
{
	(void)record;
}

void log_summary(const struct log_data_record *record)
{
	(void)record;
}

enum controller_load controller_load(
	const struct controller *controller,
	const struct controller_direction_input *input,
	uint64_t active_threshold_bps
)
{
	(void)controller;
	(void)input;
	(void)active_threshold_bps;
	return CONTROLLER_LOAD_IDLE;
}

static struct config config;
static struct daemon test_daemon = { .config = &config };
static struct latency_observation latency;

/* One upload decision: its congestion state and attribution, and what changed. */
struct upload_decision {
	enum controller_congestion_state congestion;
	bool congestion_changed;
	bool attributed;
	bool attribution_changed;
};

static void report_upload(const struct upload_decision *upload)
{
	struct shaper_step step = { .latency = &latency };
	struct controller_output *output = &step.output;
	struct controller_direction_output *decision = &output->directions[DIRECTION_UPLOAD];

	decision->congestion = upload->congestion;
	decision->congestion_changed = upload->congestion_changed;
	decision->bufferbloat_attributed = upload->attributed;
	decision->bufferbloat_attribution_changed = upload->attribution_changed;
	message_count = 0U;
	report_decision(&test_daemon, &step, DIRECTION_UPLOAD);
}

/* Upload's attribution flips while bufferbloat is detected, and when it is not. */
static const struct upload_decision flips[] = {
	{ .congestion = CONTROLLER_CONGESTION_CLEAR, .attribution_changed = true },
	{ .congestion = CONTROLLER_CONGESTION_CLEAR,
	  .attributed = true,
	  .attribution_changed = true },
	{ .congestion = CONTROLLER_CONGESTION_UNKNOWN, .attribution_changed = true },
	{ .congestion = CONTROLLER_CONGESTION_DETECTED, .attribution_changed = true },
	{ .congestion = CONTROLLER_CONGESTION_DETECTED,
	  .attributed = true,
	  .attribution_changed = true },
};

static const struct upload_decision blamed = {
	.congestion = CONTROLLER_CONGESTION_DETECTED,
	.congestion_changed = true,
	.attributed = true,
	.attribution_changed = true,
};

static const struct upload_decision spared = {
	.congestion = CONTROLLER_CONGESTION_DETECTED,
	.congestion_changed = true,
};

static const struct upload_decision cleared = {
	.congestion = CONTROLLER_CONGESTION_CLEAR,
	.congestion_changed = true,
	.attribution_changed = true,
};

/*
 * Without tcp_delay_attribution the blame comes from the delivery heuristic,
 * which flips with every traffic sample and measures nothing (issue #12): no
 * attribution line, and detection is logged as before the heuristic.
 */
static void test_without_tcp_attribution_nothing_is_logged(void)
{
	config.tcp_delay_attribution = false;
	for (size_t index = 0U; index < ARRAY_SIZE(flips); index++) {
		report_upload(&flips[index]);
		assert(message_count == 0U);
	}
	report_upload(&blamed);
	assert(message_count == 1U);
	assert(levels[0] == LOG_LEVEL_NOTICE);
	assert(strstr(messages[0], "congestion changed: direction=upload state=detected") ==
	       messages[0]);
	assert(strstr(messages[0], "attributed=") == NULL);
}

/* With measured TCP queues, a change is logged only while it decides a cut. */
static void test_tcp_attribution_change_is_logged_during_bufferbloat(void)
{
	config.tcp_delay_attribution = true;
	for (size_t index = 0U; index < 3U; index++) {
		report_upload(&flips[index]);
		assert(message_count == 0U);
	}
	report_upload(&flips[3]);
	assert(message_count == 1U);
	assert(levels[0] == LOG_LEVEL_DEBUG);
	assert(strstr(messages[0],
		      "bufferbloat attribution changed: direction=upload attributed=no") ==
	       messages[0]);
}

/* With measured TCP queues the detection message says whether the direction is blamed. */
static void test_tcp_attribution_detection_carries_it(void)
{
	config.tcp_delay_attribution = true;
	report_upload(&blamed);
	assert(message_count == 1U);
	assert(levels[0] == LOG_LEVEL_NOTICE);
	assert(strstr(messages[0], "congestion changed: direction=upload state=detected") ==
	       messages[0]);
	assert(strstr(messages[0], " attributed=yes") != NULL);

	report_upload(&spared);
	assert(message_count == 1U);
	assert(strstr(messages[0], " attributed=no") != NULL);

	/* Clearing does not depend on the attribution, so its message does not carry it. */
	report_upload(&cleared);
	assert(message_count == 1U);
	assert(levels[0] == LOG_LEVEL_INFO);
	assert(strstr(messages[0], "state=clear") != NULL);
	assert(strstr(messages[0], "attributed=") == NULL);
}

int main(void)
{
	test_without_tcp_attribution_nothing_is_logged();
	test_tcp_attribution_change_is_logged_during_bufferbloat();
	test_tcp_attribution_detection_carries_it();
	(void)puts("daemon report tests passed");
	return 0;
}
