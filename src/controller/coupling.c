#include "controller/direction.h"
#include "common/arith.h"
#include "common/rate.h"
#include "constants/controller.h"
#include "constants/units.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t coupling_download_ceiling_bps(
	const struct controller *controller,
	const struct controller_input *input
)
{
	const struct controller_config *config = &controller->config;
	const struct controller_ack_input *acks = &input->acks;
	const struct controller_direction_input *download = &input->directions[DIRECTION_DOWNLOAD];
	const struct controller_direction_input *upload = &input->directions[DIRECTION_UPLOAD];
	const struct controller_direction *shaped_upload =
		&controller->directions[DIRECTION_UPLOAD];
	uint64_t room_bps = shaped_upload->shaper_rate_bps;
	uint64_t ack_rate_bps = acks->upload_ack_rate_bps;
	uint64_t minimum_bps = ratio_of(room_bps, config->ul_congest_ack_share_ratio_e6);
	uint64_t other_bps;
	uint64_t taken_bps;
	uint64_t allowed_bps;

	if (config->ul_congest_ack_share_ratio_e6 == 0U || !acks->valid || ack_rate_bps == 0U ||
	    !upload->valid || !download->valid ||
	    load_ratio_e6(upload->traffic_rate_bps, room_bps) <=
		    config->high_load_threshold_ratio_e6) {
		return UINT64_MAX;
	}
	other_bps = saturating_sub(acks->upload_rate_bps, ack_rate_bps);
	taken_bps = other_bps + ratio_of(room_bps, UPLOAD_ACK_HEADROOM_RATIO_E6);
	allowed_bps = max_u64(saturating_sub(room_bps, taken_bps), minimum_bps);
	if (ack_rate_bps <= allowed_bps)
		return UINT64_MAX;
	return rate_holdable(mul_div(download->traffic_rate_bps, allowed_bps, ack_rate_bps));
}

bool coupling_holds_queue_share(int64_t queue_us, int64_t total_us)
{
	return queue_us > 0 &&
	       fraction_to_ratio_e6((uint64_t)queue_us, (uint64_t)total_us) >= QUEUE_SHARE_RATIO_E6;
}

uint64_t coupling_delivery_ratio_e6(
	const struct controller_direction *direction,
	const struct controller_direction_input *input
)
{
	if (!input->valid)
		return 0U;
	return load_ratio_e6(input->traffic_rate_bps, direction->shaper_rate_bps);
}

static void compensate_direction(
	struct controller_direction *direction,
	const struct controller_direction_config *configured,
	uint64_t wire_packet_bits
)
{
	struct controller_direction_config *effective = &direction->config;
	uint64_t compensation_us = serialization_us(wire_packet_bits, direction->shaper_rate_bps);

	effective->average_owd_delta_maximum_adjust_up_us =
		saturating_add(configured->average_owd_delta_maximum_adjust_up_us, compensation_us);
	effective->owd_delta_delay_threshold_us =
		saturating_add(configured->owd_delta_delay_threshold_us, compensation_us);
	effective->average_owd_delta_maximum_adjust_down_us = saturating_add(
		configured->average_owd_delta_maximum_adjust_down_us,
		compensation_us
	);
}

void controller_set_serialization_compensation(
	struct controller *controller,
	const uint64_t wire_packet_bits[DIRECTION_COUNT]
)
{
	const struct controller_config *config = &controller->config;

	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++)
		compensate_direction(
			&controller->directions[direction],
			&config->directions[direction],
			wire_packet_bits[direction]
		);
}
