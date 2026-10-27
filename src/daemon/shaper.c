#define _GNU_SOURCE

#include "daemon/report.h"
#include "daemon/state.h"

#include "common/arith.h"
#include "common/clock.h"
#include "common/rate.h"
#include "logging/log.h"
#include "logging/records.h"
#include "constants/controller.h"
#include "constants/options.h"
#include "constants/states.h"
#include "constants/text.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void shaper_update_compensation(struct daemon *daemon)
{
	struct daemon_shaper *shaper = &daemon->shaper;
	const struct daemon_links *links = &daemon->links;
	uint64_t wire_packet_bits[DIRECTION_COUNT];

	if (!links_wire_ready(daemon))
		return;
	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
		const struct links_direction *link = &links->directions[direction];

		wire_packet_bits[direction] = cake_max_wire_packet_bits(&link->cake);
	}
	controller_set_serialization_compensation(&shaper->controller, wire_packet_bits);
}

static void apply_bandwidth(
	struct daemon *daemon,
	struct links_direction *direction,
	uint64_t desired_rate_bps,
	enum controller_rate_reason reason
)
{
	const struct cake_observation *cake = &direction->cake;
	/* Seed the readback with the known index and MTU so neither is re-queried. */
	struct cake_observation verified = *cake;
	struct cake_read readback = { .interface = direction->interface, .observation = &verified };
	char error[ERROR_SIZE] = { 0 };

	if (daemon->config->output_cake_changes)
		log_shaper(direction->interface, desired_rate_bps);

	if (cake_set_bandwidth(
		    &daemon->netlink,
		    &direction->cake,
		    desired_rate_bps,
		    error,
		    sizeof(error)
	    ) != 0) {
		log_message(
			LOG_LEVEL_WARNING,
			"CAKE bandwidth change failed: direction=%s interface=%s"
			" old_rate=%" PRIu64 " bit/s desired_rate=%" PRIu64 " bit/s reason=%s: %s",
			direction->name,
			direction->interface,
			cake->bandwidth_bps,
			desired_rate_bps,
			report_rate_reason_name(reason),
			error
		);
		return;
	}

	cake_read(&daemon->netlink, &readback, 1U);
	if (readback.result != CAKE_READ_FOUND || !verified.has_bandwidth ||
	    verified.bandwidth_bps != desired_rate_bps) {
		log_message(
			LOG_LEVEL_WARNING,
			"CAKE bandwidth verification failed: direction=%s interface=%s"
			" desired_rate=%" PRIu64 " bit/s result=%s",
			direction->name,
			direction->interface,
			desired_rate_bps,
			readback.result == CAKE_READ_ERROR ? readback.error : READBACK_MISMATCH
		);
		return;
	}

	direction->cake = verified;
}

static struct controller_direction_input direction_input(const struct links_direction *direction)
{
	const struct cake_observation *cake = &direction->cake;
	const struct controller_direction_input input = {
		.traffic_sample_id = direction->traffic_sample_id,
		.valid = direction->traffic_valid && direction->cake_valid && cake->has_bandwidth &&
			 cake->bandwidth_bps > 0U,
		.traffic_rate_bps = direction->traffic_rate_bps,
		.cake_rate_bps = direction->cake_valid ? cake->bandwidth_bps : 0U,
	};

	return input;
}

bool shaper_low_load(const struct daemon *daemon)
{
	const struct daemon_shaper *shaper = &daemon->shaper;
	const struct daemon_links *links = &daemon->links;
	struct controller_direction_input inputs[DIRECTION_COUNT];

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++)
		inputs[direction] = direction_input(&links->directions[direction]);
	return controller_low_load(&shaper->controller, inputs);
}

void shaper_update(
	struct daemon *daemon,
	const struct latency_observation *latency,
	const struct latency_sample *sample
)
{
	const struct config *config = daemon->config;
	struct daemon_shaper *shaper = &daemon->shaper;
	struct controller *controller = &shaper->controller;
	struct daemon_links *links = &daemon->links;
	struct shaper_step step = { .latency = latency, .sample = sample };
	struct controller_input *input = &step.input;
	struct controller_output *output = &step.output;

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
		input->directions[direction] = direction_input(&links->directions[direction]);
		input->latency[direction] = (struct controller_latency_input){
			.valid = true,
			.owd_delta_us = latency->owd_delta_us[direction],
		};
	}
	(void)read_clock_us(CLOCK_MONOTONIC, &input->timestamp_us);
	tcp_observe(daemon, input);

	controller_update(controller, input, output);
	for (size_t index = 0U; index < DIRECTION_COUNT; index++) {
		enum direction direction = (enum direction)index;
		struct links_direction *link = &links->directions[direction];
		const struct controller_direction_output *decision = &output->directions[direction];
		const struct controller_direction *controlled = &controller->directions[direction];
		const struct controller_direction_config *limits = &controlled->config;

		report_decision(daemon, &step, direction);
		/* cake-autorate's first set_shaper_rates() reports a non-adjusted base rate too. */
		if (!shaper->initial_shaper_reported && !limits->adjust &&
		    config->output_cake_changes) {
			log_shaper(link->interface, limits->base_rate_bps);
			log_message(
				LOG_LEVEL_DEBUG,
				"adjust_%s_shaper_rate set to 0 in config, so skipping the corresponding tc qdisc change call.",
				direction_short_name(direction)
			);
		}
		/* The controller never requests changes for an observation-only link. */
		if (decision->rate_changed)
			apply_bandwidth(daemon, link, decision->rate_bps, decision->rate_reason);
	}
	shaper->initial_shaper_reported = true;
	shaper_update_compensation(daemon);
	report_records(daemon, &step);
}

void shaper_enforce_minimum(struct daemon *daemon, uint64_t timestamp_us)
{
	struct daemon_shaper *shaper = &daemon->shaper;
	struct controller *controller = &shaper->controller;
	struct daemon_links *links = &daemon->links;

	controller_set_minimum_rates(controller, timestamp_us);
	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++) {
		const struct controller_direction *controlled = &controller->directions[direction];
		const struct controller_direction_config *config = &controlled->config;
		struct links_direction *link = &links->directions[direction];

		if (!config->adjust || !link->cake_valid)
			continue;
		apply_bandwidth(daemon, link, config->minimum_rate_bps, CONTROLLER_RATE_RECONCILE);
	}
}

static struct controller_direction_config direction_config(const struct config_direction *direction)
{
	return (struct controller_direction_config){
		.adjust = direction->adjust,
		.minimum_rate_bps = direction->minimum_rate_bps,
		.base_rate_bps = direction->base_rate_bps,
		.maximum_rate_bps = direction->maximum_rate_bps,
		.average_owd_delta_maximum_adjust_up_us =
			direction->average_owd_delta_maximum_adjust_up_us,
		.owd_delta_delay_threshold_us = direction->owd_delta_delay_threshold_us,
		.average_owd_delta_maximum_adjust_down_us =
			direction->average_owd_delta_maximum_adjust_down_us,
	};
}

int shaper_start(struct daemon *daemon)
{
	const struct config *config = daemon->config;
	struct controller_config controller_config = {
		.bufferbloat_detection_window = (unsigned int)config->bufferbloat_detection_window,
		.bufferbloat_detection_threshold =
			(unsigned int)config->bufferbloat_detection_threshold,
		.rate_minimum_adjust_down_bufferbloat_ratio_e6 =
			config->shaper_rate_minimum_adjust_down_bufferbloat_ratio_e6,
		.rate_maximum_adjust_down_bufferbloat_ratio_e6 =
			config->shaper_rate_maximum_adjust_down_bufferbloat_ratio_e6,
		.rate_minimum_adjust_up_load_high_ratio_e6 =
			config->shaper_rate_minimum_adjust_up_load_high_ratio_e6,
		.rate_maximum_adjust_up_load_high_ratio_e6 =
			config->shaper_rate_maximum_adjust_up_load_high_ratio_e6,
		.rate_adjust_down_load_low_ratio_e6 =
			config->shaper_rate_adjust_down_load_low_ratio_e6,
		.rate_adjust_up_load_low_ratio_e6 = config->shaper_rate_adjust_up_load_low_ratio_e6,
		.high_load_threshold_ratio_e6 = config->high_load_threshold_ratio_e6,
		.bufferbloat_refractory_period_us = config->bufferbloat_refractory_period_us,
		.decay_refractory_period_us = config->decay_refractory_period_us,
		.shared_delay = pinger_shared_delay(daemon->pinger_method),
		.ul_congest_ack_share_ratio_e6 = config->ul_congest_ack_share_ratio_e6,
	};
	struct daemon_shaper *shaper = &daemon->shaper;

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++)
		controller_config.directions[direction] =
			direction_config(&config->directions[direction]);
	if (controller_init(&shaper->controller, &controller_config) != 0) {
		log_message(LOG_LEVEL_ERROR, "could not initialize controller: %s", strerror(errno));
		return -1;
	}
	return 0;
}
