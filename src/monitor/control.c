#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define LOAD_CONDITION_SIZE 16U

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

void control_update_compensation(struct monitor *monitor)
{
	struct controller *controller = &monitor->control.controller;

	if (!links_wire_ready(monitor))
		return;
	controller_set_serialization_compensation(
		controller,
		cake_max_wire_packet_bits(&monitor->links.download.cake),
		cake_max_wire_packet_bits(&monitor->links.upload.cake),
		controller->download.shaper_rate_bits_per_second,
		controller->upload.shaper_rate_bits_per_second
	);
}

static void log_line_state(
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
		input->traffic_rate_bits_per_second,
		input->cake_rate_bits_per_second
	);
}

static void log_congestion_state(
	const char *direction,
	enum controller_congestion_state state,
	const struct latency_observation *latency
)
{
	int64_t round_trip_microseconds;
	int64_t baseline_microseconds;
	int64_t delay_microseconds;

	if (state == CONTROLLER_CONGESTION_UNKNOWN) {
		log_message(
			LOG_LEVEL_WARNING,
			"congestion observation unavailable: direction=%s",
			direction
		);
		return;
	}

	round_trip_microseconds =
		latency->download_owd_microseconds + latency->upload_owd_microseconds;
	baseline_microseconds = latency->download_owd_baseline_microseconds +
				latency->upload_owd_baseline_microseconds;
	delay_microseconds = round_trip_microseconds >= baseline_microseconds ?
				     round_trip_microseconds - baseline_microseconds :
				     0;
	log_message(
		state == CONTROLLER_CONGESTION_DETECTED ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO,
		"congestion changed: direction=%s state=%s"
		" rtt=%" PRId64 " us baseline=%" PRId64 " us"
		" delta=%" PRId64 " us",
		direction,
		congestion_state_names[state],
		round_trip_microseconds,
		baseline_microseconds,
		delay_microseconds
	);
}

static const char *const load_names[] = {
	[CONTROLLER_LOAD_IDLE] = STATE_IDLE,
	[CONTROLLER_LOAD_LOW] = STATE_LOW,
	[CONTROLLER_LOAD_HIGH] = STATE_HIGH,
};

/* The DATA and SUMMARY load column, such as "dl_high_bb". */
static void load_condition(
	char *condition,
	size_t condition_size,
	const char *direction,
	enum controller_load load,
	enum controller_congestion_state congestion
)
{
	(void)snprintf(
		condition,
		condition_size,
		"%s_%s%s",
		direction,
		load_names[load],
		congestion == CONTROLLER_CONGESTION_DETECTED ? BUFFERBLOAT_SUFFIX : EMPTY_STRING
	);
}

static void log_controller_stats(
	const struct config *config,
	const struct controller *controller,
	const struct controller_input *input,
	const struct controller_output *output,
	const struct latency_observation *latency,
	const struct latency_sample *sample
)
{
	/* Records report the serialization-compensated thresholds in effect. */
	const struct controller_direction_config *download_effective = &controller->download.config;
	const struct controller_direction_config *upload_effective = &controller->upload.config;
	char download_condition[LOAD_CONDITION_SIZE];
	char upload_condition[LOAD_CONDITION_SIZE];
	uint64_t download_rate = output->download.rate_bits_per_second / KILOBIT;
	uint64_t upload_rate = output->upload.rate_bits_per_second / KILOBIT;
	unsigned int download_load;
	unsigned int upload_load;

	if (!config->output_processing_stats && !config->output_summary_stats)
		return;
	download_load = load_percent(
		input->download.traffic_rate_bits_per_second,
		input->download.cake_rate_bits_per_second
	);
	upload_load = load_percent(
		input->upload.traffic_rate_bits_per_second,
		input->upload.cake_rate_bits_per_second
	);

	load_condition(
		download_condition,
		sizeof(download_condition),
		DIRECTION_DOWNLOAD_SHORT,
		controller_load(
			controller,
			&input->download,
			config->connection_active_threshold_bits_per_second
		),
		output->download.congestion
	);
	load_condition(
		upload_condition,
		sizeof(upload_condition),
		DIRECTION_UPLOAD_SHORT,
		controller_load(
			controller,
			&input->upload,
			config->connection_active_threshold_bits_per_second
		),
		output->upload.congestion
	);

	if (config->output_processing_stats) {
		/*
		 * Standard fping supplies RTT rather than directional timestamps.
		 * Match cake-autorate's fping path by recording the same half-RTT
		 * estimate in its separate download and upload OWD columns.
		 */
		const struct log_data_record record = {
			.download_achieved_rate_kbps =
				input->download.traffic_rate_bits_per_second / KILOBIT,
			.upload_achieved_rate_kbps =
				input->upload.traffic_rate_bits_per_second / KILOBIT,
			.download_load_percent = download_load,
			.upload_load_percent = upload_load,
			.icmp_timestamp = sample->timestamp_text,
			.reflector = sample->target,
			.sequence = latency->sequence,
			.download_owd_baseline_microseconds =
				latency->download_owd_baseline_microseconds,
			.download_owd_microseconds = latency->download_owd_microseconds,
			.download_owd_delta_ewma_microseconds =
				latency->download_owd_delta_ewma_microseconds,
			.download_owd_delta_microseconds = latency->download_owd_delta_microseconds,
			.download_adjust_delay_threshold_microseconds =
				download_effective->delay_threshold_microseconds,
			.upload_owd_baseline_microseconds =
				latency->upload_owd_baseline_microseconds,
			.upload_owd_microseconds = latency->upload_owd_microseconds,
			.upload_owd_delta_ewma_microseconds =
				latency->upload_owd_delta_ewma_microseconds,
			.upload_owd_delta_microseconds = latency->upload_owd_delta_microseconds,
			.upload_adjust_delay_threshold_microseconds =
				upload_effective->delay_threshold_microseconds,
			.download_sum_delays = output->download.delayed_sample_count,
			.download_average_owd_delta_microseconds =
				output->download.average_delay_microseconds,
			.download_maximum_adjust_up_threshold_microseconds =
				download_effective->average_delay_maximum_adjust_up_microseconds,
			.download_maximum_adjust_down_threshold_microseconds =
				download_effective->average_delay_maximum_adjust_down_microseconds,
			.upload_sum_delays = output->upload.delayed_sample_count,
			.upload_average_owd_delta_microseconds =
				output->upload.average_delay_microseconds,
			.upload_maximum_adjust_up_threshold_microseconds =
				upload_effective->average_delay_maximum_adjust_up_microseconds,
			.upload_maximum_adjust_down_threshold_microseconds =
				upload_effective->average_delay_maximum_adjust_down_microseconds,
			.download_load_condition = download_condition,
			.upload_load_condition = upload_condition,
			.cake_download_rate_kbps = download_rate,
			.cake_upload_rate_kbps = upload_rate,
		};

		log_data(&record);
	}

	if (config->output_summary_stats) {
		const struct log_summary_record record = {
			.download_achieved_rate_kbps =
				input->download.traffic_rate_bits_per_second / KILOBIT,
			.upload_achieved_rate_kbps =
				input->upload.traffic_rate_bits_per_second / KILOBIT,
			.download_sum_delays = output->download.delayed_sample_count,
			.upload_sum_delays = output->upload.delayed_sample_count,
			.download_average_owd_delta_microseconds =
				output->download.average_delay_microseconds,
			.upload_average_owd_delta_microseconds =
				output->upload.average_delay_microseconds,
			.download_load_condition = download_condition,
			.upload_load_condition = upload_condition,
			.cake_download_rate_kbps = download_rate,
			.cake_upload_rate_kbps = upload_rate,
		};

		log_summary(&record);
	}
}

static void apply_bandwidth(
	struct netlink *netlink,
	struct monitor_direction *direction,
	uint64_t desired_rate,
	enum controller_rate_reason reason,
	bool output_cake_changes
)
{
	/* Seed the readback with the known index and MTU so neither is re-queried. */
	struct cake_observation verified = direction->cake;
	char error[ERROR_SIZE] = { 0 };
	enum cake_read_result read_result;

	if (output_cake_changes)
		log_shaper(direction->interface, desired_rate / KILOBIT);

	if (cake_set_bandwidth(netlink, &direction->cake, desired_rate, error, sizeof(error)) !=
	    0) {
		log_message(
			LOG_LEVEL_WARNING,
			"CAKE bandwidth change failed: direction=%s interface=%s"
			" old_rate=%" PRIu64 " bit/s desired_rate=%" PRIu64 " bit/s reason=%s: %s",
			direction->name,
			direction->interface,
			direction->cake.bandwidth_bits_per_second,
			desired_rate,
			rate_reason_names[reason],
			error
		);
		return;
	}

	read_result = cake_read(netlink, direction->interface, &verified, error, sizeof(error));
	if (read_result != CAKE_READ_FOUND || !verified.has_bandwidth ||
	    verified.bandwidth_bits_per_second != desired_rate) {
		log_message(
			LOG_LEVEL_WARNING,
			"CAKE bandwidth verification failed: direction=%s interface=%s"
			" desired_rate=%" PRIu64 " bit/s result=%s",
			direction->name,
			direction->interface,
			desired_rate,
			read_result == CAKE_READ_ERROR ? error : READBACK_MISMATCH
		);
		return;
	}

	direction->cake = verified;
}

static struct controller_direction_input direction_input(const struct monitor_direction *direction)
{
	const struct controller_direction_input input = {
		.traffic_sample_id = direction->traffic_sample_id,
		.valid = direction->traffic_valid && direction->cake_valid &&
			 direction->cake.has_bandwidth &&
			 direction->cake.bandwidth_bits_per_second > 0U,
		.traffic_rate_bits_per_second = direction->traffic_rate_bits_per_second,
		.cake_rate_bits_per_second =
			direction->cake_valid ? direction->cake.bandwidth_bits_per_second : 0U,
	};

	return input;
}

bool control_low_load(const struct monitor *monitor)
{
	const struct controller_direction_input download =
		direction_input(&monitor->links.download);
	const struct controller_direction_input upload = direction_input(&monitor->links.upload);

	return controller_low_load(&monitor->control.controller, &download, &upload);
}

void control_update(
	struct monitor *monitor,
	const struct latency_observation *latency,
	const struct latency_sample *sample
)
{
	const struct config *config = monitor->config;
	struct monitor_control *control = &monitor->control;
	struct monitor_links *links = &monitor->links;
	struct controller_input input = {
		.download = direction_input(&links->download),
		.upload = direction_input(&links->upload),
		.download_latency = { .valid = true,
				      .owd_delta_microseconds =
					      latency->download_owd_delta_microseconds },
		.upload_latency = { .valid = true,
				    .owd_delta_microseconds =
					    latency->upload_owd_delta_microseconds },
		.timestamp_microseconds = 0U,
	};
	struct controller_output output;
	const struct {
		struct monitor_direction *direction;
		const struct controller_direction *controller;
		const struct controller_direction_input *input;
		const struct controller_direction_output *output;
		const char *short_name;
	} directions[] = {
		{ &links->download,
		  &control->controller.download,
		  &input.download,
		  &output.download,
		  DIRECTION_DOWNLOAD_SHORT },
		{ &links->upload,
		  &control->controller.upload,
		  &input.upload,
		  &output.upload,
		  DIRECTION_UPLOAD_SHORT },
	};

	(void)read_clock_microseconds(CLOCK_MONOTONIC, &input.timestamp_microseconds);
	tcp_observe(monitor, input.timestamp_microseconds, &input.queue, &input.acks);

	controller_update(&control->controller, &input, &output);
	for (size_t index = 0U; index < ARRAY_SIZE(directions); index++) {
		struct monitor_direction *direction = directions[index].direction;
		const struct controller_direction_output *decision = directions[index].output;

		if (decision->state_changed)
			log_line_state(direction->name, decision->state, directions[index].input);
		if (decision->congestion_changed)
			log_congestion_state(direction->name, decision->congestion, latency);
		if (decision->bufferbloat_attribution_changed) {
			log_message(
				LOG_LEVEL_DEBUG,
				"bufferbloat attribution changed: direction=%s attributed=%s"
				" tcp_queues=%s download_queue=%" PRId64 " us upload_queue=%" PRId64
				" us",
				direction->name,
				decision->bufferbloat_attributed ? "yes" : "no",
				input.queue.valid ? "valid" : "unavailable",
				input.queue.download_microseconds,
				input.queue.upload_microseconds
			);
		}
		/* cake-autorate's first set_shaper_rates() reports a non-adjusted base rate too. */
		if (!control->initial_shaper_reported &&
		    !directions[index].controller->config.adjust && config->output_cake_changes) {
			log_shaper(
				direction->interface,
				directions[index].controller->config.base_rate_bits_per_second /
					KILOBIT
			);
			log_message(
				LOG_LEVEL_DEBUG,
				"adjust_%s_shaper_rate set to 0 in config, so skipping the corresponding tc qdisc change call.",
				directions[index].short_name
			);
		}
		/* The controller never requests changes for an observation-only link. */
		if (decision->rate_changed) {
			apply_bandwidth(
				&monitor->netlink,
				direction,
				decision->rate_bits_per_second,
				decision->rate_reason,
				config->output_cake_changes
			);
		}
	}
	control->initial_shaper_reported = true;
	control_update_compensation(monitor);
	log_controller_stats(config, &control->controller, &input, &output, latency, sample);
}

void control_enforce_minimum(struct monitor *monitor, uint64_t timestamp_microseconds)
{
	const struct config *config = monitor->config;
	struct monitor_links *links = &monitor->links;

	controller_set_minimum_rates(&monitor->control.controller, timestamp_microseconds);
	if (config->adjust_download && links->download.cake_valid) {
		apply_bandwidth(
			&monitor->netlink,
			&links->download,
			config->minimum_download_rate_bits_per_second,
			CONTROLLER_RATE_RECONCILE,
			config->output_cake_changes
		);
	}
	if (config->adjust_upload && links->upload.cake_valid) {
		apply_bandwidth(
			&monitor->netlink,
			&links->upload,
			config->minimum_upload_rate_bits_per_second,
			CONTROLLER_RATE_RECONCILE,
			config->output_cake_changes
		);
	}
}

int control_start(struct monitor *monitor)
{
	const struct config *config = monitor->config;
	/* Match cake-autorate's startup rounding to per-thousand and percent. */
	const struct controller_config controller_config = {
		.download = { .adjust = config->adjust_download,
			      .minimum_rate_bits_per_second =
				      config->minimum_download_rate_bits_per_second,
			      .base_rate_bits_per_second =
				      config->base_download_rate_bits_per_second,
			      .maximum_rate_bits_per_second =
				      config->maximum_download_rate_bits_per_second,
			      .average_delay_maximum_adjust_up_microseconds =
				      config->download_average_owd_delta_maximum_adjust_up_microseconds,
			      .delay_threshold_microseconds =
				      config->download_owd_delta_delay_threshold_microseconds,
			      .average_delay_maximum_adjust_down_microseconds =
				      config->download_average_owd_delta_maximum_adjust_down_microseconds },
		.upload = { .adjust = config->adjust_upload,
			    .minimum_rate_bits_per_second =
				    config->minimum_upload_rate_bits_per_second,
			    .base_rate_bits_per_second = config->base_upload_rate_bits_per_second,
			    .maximum_rate_bits_per_second =
				    config->maximum_upload_rate_bits_per_second,
			    .average_delay_maximum_adjust_up_microseconds =
				    config->upload_average_owd_delta_maximum_adjust_up_microseconds,
			    .delay_threshold_microseconds =
				    config->upload_owd_delta_delay_threshold_microseconds,
			    .average_delay_maximum_adjust_down_microseconds =
				    config->upload_average_owd_delta_maximum_adjust_down_microseconds },
		.bufferbloat_detection_window = (unsigned int)config->bufferbloat_detection_window,
		.bufferbloat_detection_threshold =
			(unsigned int)config->bufferbloat_detection_threshold,
		.rate_minimum_adjust_down_bufferbloat_per_thousand = rounded_divide(
			config->shaper_rate_minimum_adjust_down_bufferbloat_per_million,
			THOUSAND
		),
		.rate_maximum_adjust_down_bufferbloat_per_thousand = rounded_divide(
			config->shaper_rate_maximum_adjust_down_bufferbloat_per_million,
			THOUSAND
		),
		.rate_minimum_adjust_up_high_load_per_thousand = rounded_divide(
			config->shaper_rate_minimum_adjust_up_load_high_per_million,
			THOUSAND
		),
		.rate_maximum_adjust_up_high_load_per_thousand = rounded_divide(
			config->shaper_rate_maximum_adjust_up_load_high_per_million,
			THOUSAND
		),
		.rate_adjust_down_low_load_per_thousand = rounded_divide(
			config->shaper_rate_adjust_down_load_low_per_million,
			THOUSAND
		),
		.rate_adjust_up_low_load_per_thousand =
			rounded_divide(config->shaper_rate_adjust_up_load_low_per_million, THOUSAND),
		.high_load_threshold_percent =
			rounded_divide(config->high_load_threshold_per_million, FACTOR_PER_PERCENT),
		.bufferbloat_refractory_period_microseconds =
			config->bufferbloat_refractory_period_microseconds,
		.decay_refractory_period_microseconds =
			config->decay_refractory_period_microseconds,
		/* Only fping reports one RTT/2 delay for both directions. */
		.shared_delay = strcmp(config->pinger_method, PINGER_METHOD_FPING) == 0,
		.upload_ack_share_min_percent =
			rounded_divide(config->upload_ack_share_min_per_million, FACTOR_PER_PERCENT),
	};

	if (controller_init(&monitor->control.controller, &controller_config) != 0) {
		log_message(LOG_LEVEL_ERROR, "could not initialize controller: %s", strerror(errno));
		return -1;
	}
	return 0;
}
