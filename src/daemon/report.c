#define _GNU_SOURCE

#include "daemon/report.h"
#include "daemon/state.h"

#include "common/arith.h"
#include "common/rate.h"
#include "logging/log.h"
#include "logging/records.h"
#include "constants/controller.h"
#include "constants/states.h"
#include "constants/text.h"

#include <inttypes.h>
#include <stdio.h>

/* Indexed by the controller's own enum values. */
static const char *const line_state_names[] = {
	[CONTROLLER_LINE_UNKNOWN] = STATE_UNKNOWN,
	[CONTROLLER_LINE_BELOW_CAPACITY] = STATE_BELOW_CAPACITY,
	[CONTROLLER_LINE_SATURATED] = STATE_SATURATED,
};

static const char *const congestion_state_names[] = {
	[CONTROLLER_CONGESTION_UNKNOWN] = STATE_UNKNOWN,
	[CONTROLLER_CONGESTION_CLEAR] = STATE_CLEAR,
	[CONTROLLER_CONGESTION_DETECTED] = STATE_DETECTED,
};

static const char *const rate_reason_names[] = {
	[CONTROLLER_RATE_UNCHANGED] = STATE_UNCHANGED,
	[CONTROLLER_RATE_INITIAL] = STATE_INITIAL,
	[CONTROLLER_RATE_CONGESTION] = STATE_CONGESTION,
	[CONTROLLER_RATE_HIGH_LOAD] = STATE_HIGH_LOAD,
	[CONTROLLER_RATE_RETURN_TO_BASE] = STATE_RETURN_TO_BASE,
	[CONTROLLER_RATE_RECONCILE] = STATE_RECONCILE,
	[CONTROLLER_RATE_ACK_SHARE] = STATE_ACK_SHARE,
};

const char *report_rate_reason_name(enum controller_rate_reason reason)
{
	return rate_reason_names[reason];
}

static void report_line_state(
	const char *direction,
	enum controller_line_state state,
	const struct controller_direction_input *input
)
{
	if (state == CONTROLLER_LINE_UNKNOWN) {
		log_message(LOG_LEVEL_WARNING, "line load unavailable: direction=%s", direction);
		return;
	}

	log_message(
		state == CONTROLLER_LINE_SATURATED ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO,
		"line load changed: direction=%s state=%s"
		" traffic_rate=%" PRIu64 " bit/s"
		" cake_rate=%" PRIu64 " bit/s",
		direction,
		line_state_names[state],
		input->traffic_rate_bps,
		input->cake_rate_bps
	);
}

static void report_congestion_state(
	const char *direction,
	enum controller_congestion_state state,
	const struct latency_observation *latency
)
{
	int64_t round_trip_us;
	int64_t baseline_us;
	int64_t delay_us;

	if (state == CONTROLLER_CONGESTION_UNKNOWN) {
		log_message(
			LOG_LEVEL_WARNING,
			"congestion observation unavailable: direction=%s",
			direction
		);
		return;
	}

	round_trip_us = latency->owd_us[DIRECTION_DOWNLOAD] + latency->owd_us[DIRECTION_UPLOAD];
	baseline_us = latency->owd_baseline_us[DIRECTION_DOWNLOAD] +
		      latency->owd_baseline_us[DIRECTION_UPLOAD];
	delay_us = max_i64(signed_difference(round_trip_us, baseline_us), 0);
	log_message(
		state == CONTROLLER_CONGESTION_DETECTED ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO,
		"congestion changed: direction=%s state=%s"
		" rtt=%" PRId64 " us baseline=%" PRId64 " us"
		" delta=%" PRId64 " us",
		direction,
		congestion_state_names[state],
		round_trip_us,
		baseline_us,
		delay_us
	);
}

static const char *const load_names[] = {
	[CONTROLLER_LOAD_IDLE] = STATE_IDLE,
	[CONTROLLER_LOAD_LOW] = STATE_LOW,
	[CONTROLLER_LOAD_HIGH] = STATE_HIGH,
};

/* The DATA and SUMMARY load column, such as "dl_high_bb". */
static void load_condition(
	char condition[CONTROLLER_LOAD_CONDITION_SIZE],
	const char *direction,
	enum controller_load load,
	enum controller_congestion_state congestion
)
{
	(void)snprintf(
		condition,
		CONTROLLER_LOAD_CONDITION_SIZE,
		"%s_%s%s",
		direction,
		load_names[load],
		congestion == CONTROLLER_CONGESTION_DETECTED ? BUFFERBLOAT_SUFFIX : EMPTY_STRING
	);
}

/* Only the start and end of a hold: the ceiling itself moves with traffic. */
static void report_ack_share(
	const char *direction,
	const struct controller_direction_output *decision,
	const struct controller_input *input,
	const struct controller_direction *upload
)
{
	const struct controller_ack_input *acks = &input->acks;

	if (!decision->ack_share_active) {
		log_message(LOG_LEVEL_DEBUG, "ACK share released %s", direction);
		return;
	}
	log_message(
		LOG_LEVEL_DEBUG,
		"ACK share holding %s: ceiling=%" PRIu64 " bit/s ack_rate=%" PRIu64
		" bit/s upload_rate=%" PRIu64 " bit/s upload_shaper_rate=%" PRIu64 " bit/s",
		direction,
		decision->ack_share_ceiling_bps,
		acks->upload_ack_rate_bps,
		acks->upload_rate_bps,
		upload->shaper_rate_bps
	);
}

void report_decision(
	const struct daemon *daemon,
	const struct shaper_step *step,
	enum direction direction
)
{
	const struct daemon_shaper *shaper = &daemon->shaper;
	const struct controller *controller = &shaper->controller;
	const struct controller_input *input = &step->input;
	const struct controller_output *output = &step->output;
	const struct controller_queue_input *queue = &input->queue;
	const struct controller_direction_output *decision = &output->directions[direction];
	const char *name = direction_name(direction);

	if (decision->state_changed)
		report_line_state(name, decision->state, &input->directions[direction]);
	if (decision->congestion_changed)
		report_congestion_state(name, decision->congestion, step->latency);
	if (decision->bufferbloat_attribution_changed) {
		log_message(
			LOG_LEVEL_DEBUG,
			"bufferbloat attribution changed: direction=%s attributed=%s"
			" tcp_queues=%s download_queue=%" PRId64 " us upload_queue=%" PRId64 " us",
			name,
			decision->bufferbloat_attributed ? BOOLEAN_YES : BOOLEAN_NO,
			queue->valid ? STATE_VALID : STATE_UNAVAILABLE,
			queue->delay_us[DIRECTION_DOWNLOAD],
			queue->delay_us[DIRECTION_UPLOAD]
		);
	}
	if (decision->ack_share_changed)
		report_ack_share(name, decision, input, &controller->directions[DIRECTION_UPLOAD]);
}

void report_records(const struct daemon *daemon, const struct shaper_step *step)
{
	const struct config *config = daemon->config;
	const struct daemon_shaper *shaper = &daemon->shaper;
	const struct controller *controller = &shaper->controller;
	const struct controller_input *inputs = &step->input;
	const struct controller_output *outputs = &step->output;
	const struct latency_observation *latency = step->latency;
	const struct latency_sample *sample = step->sample;
	char conditions[DIRECTION_COUNT][CONTROLLER_LOAD_CONDITION_SIZE];
	/*
	 * Echo pingers (native, fping) supply RTT rather than directional
	 * timestamps. Match cake-autorate's fping path by recording the same
	 * half-RTT estimate in its separate download and upload OWD columns.
	 */
	struct log_data_record record = {
		.icmp_timestamp = sample->timestamp_text,
		.reflector = sample->target,
		.sequence = sample->sequence,
	};

	if (!config->output_processing_stats && !config->output_summary_stats)
		return;
	for (size_t index = 0U; index < DIRECTION_COUNT; index++) {
		enum direction direction = (enum direction)index;
		const struct controller_direction_input *input = &inputs->directions[direction];
		const struct controller_direction_output *output = &outputs->directions[direction];
		const struct controller_direction *controlled = &controller->directions[direction];
		/* Records report the serialization-compensated thresholds in effect. */
		const struct controller_direction_config *effective = &controlled->config;

		load_condition(
			conditions[direction],
			direction_short_name(direction),
			controller_load(controller, input, config->connection_active_threshold_bps),
			output->congestion
		);
		record.achieved_rate_bps[direction] = input->traffic_rate_bps;
		record.load_ratio_e6[direction] =
			load_ratio_e6(input->traffic_rate_bps, input->cake_rate_bps);
		record.owd_baseline_us[direction] = latency->owd_baseline_us[direction];
		record.owd_us[direction] = latency->owd_us[direction];
		record.owd_delta_ewma_us[direction] = latency->owd_delta_ewma_us[direction];
		record.owd_delta_us[direction] = latency->owd_delta_us[direction];
		record.adjust_delay_threshold_us[direction] =
			effective->owd_delta_delay_threshold_us;
		record.sum_delays[direction] = output->delayed_sample_count;
		record.average_owd_delta_us[direction] = output->average_delay_us;
		record.maximum_adjust_up_threshold_us[direction] =
			effective->average_owd_delta_maximum_adjust_up_us;
		record.maximum_adjust_down_threshold_us[direction] =
			effective->average_owd_delta_maximum_adjust_down_us;
		record.load_condition[direction] = conditions[direction];
		record.cake_rate_bps[direction] = output->rate_bps;
	}
	if (config->output_processing_stats)
		log_data(&record);
	if (config->output_summary_stats)
		log_summary(&record);
}
