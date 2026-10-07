#include "config/defaults.h"
#include "config/config.h"
#include "common/constants.h"

#include <string.h>

void defaults_apply(struct config *config)
{
	*config = (struct config){
		/* Adjustment remains opt-in even though cake-autorate defaults it on. */
		.download.adjust = false,
		.upload.adjust = false,
		.debug = true,
		.log_to_file = true,
		.randomize_reflectors = true,
		.retain_reflector_stats = true,
		.enable_sleep_function = true,
		.log_file_export_compress = true,
		/* Not in cake-autorate; opt-in until verified on real lines. */
		.tcp_delay_attribution = false,
		/* Not in cake-autorate; off until measured on real lines. */
		.ul_congest_ack_share_ratio_e6 = 0U,
		.log_file_max_time_us = 10U * MINUTE,
		.log_file_max_size_bytes = 2000U * KILOBYTE,
		.no_pingers = 6U,
		.reflector_ping_interval_us = 300U * MILLISECOND,
		.download.average_owd_delta_maximum_adjust_up_us = 10U * MILLISECOND,
		.upload.average_owd_delta_maximum_adjust_up_us = 10U * MILLISECOND,
		.download.owd_delta_delay_threshold_us = 30U * MILLISECOND,
		.upload.owd_delta_delay_threshold_us = 30U * MILLISECOND,
		.download.average_owd_delta_maximum_adjust_down_us = 60U * MILLISECOND,
		.upload.average_owd_delta_maximum_adjust_down_us = 60U * MILLISECOND,
		.download.minimum_rate_bps = 5U * MEGABIT,
		.download.base_rate_bps = 20U * MEGABIT,
		.download.maximum_rate_bps = 80U * MEGABIT,
		.upload.minimum_rate_bps = 5U * MEGABIT,
		.upload.base_rate_bps = 20U * MEGABIT,
		.upload.maximum_rate_bps = 35U * MEGABIT,
		.connection_active_threshold_bps = 2U * MEGABIT,
		.sustained_idle_sleep_threshold_us = MINUTE,
		.log_file_buffer_timeout_us = 500U * MILLISECOND,
		.irtt_session_duration_us = 10U * MINUTE,
		.monitor_achieved_rates_interval_us = 200U * MILLISECOND,
		.monitor_cpu_usage_interval_us = 2U * SECOND,
		.bufferbloat_detection_window = 6U,
		.bufferbloat_detection_threshold = 3U,
		.alpha_baseline_increase_ratio_e6 = RATIO_ONE_E6 / 1000U,
		.alpha_baseline_decrease_ratio_e6 = 90U * RATIO_PERCENT_E6,
		.alpha_delta_ewma_ratio_e6 = 95U * RATIO_ONE_E6 / 1000U,
		.shaper_rate_minimum_adjust_down_bufferbloat_ratio_e6 = 99U * RATIO_PERCENT_E6,
		.shaper_rate_maximum_adjust_down_bufferbloat_ratio_e6 = 75U * RATIO_PERCENT_E6,
		.shaper_rate_minimum_adjust_up_load_high_ratio_e6 = RATIO_ONE_E6,
		.shaper_rate_maximum_adjust_up_load_high_ratio_e6 = 104U * RATIO_PERCENT_E6,
		.shaper_rate_adjust_down_load_low_ratio_e6 = 99U * RATIO_PERCENT_E6,
		.shaper_rate_adjust_up_load_low_ratio_e6 = 101U * RATIO_PERCENT_E6,
		.high_load_threshold_ratio_e6 = 75U * RATIO_PERCENT_E6,
		.bufferbloat_refractory_period_us = 300U * MILLISECOND,
		.decay_refractory_period_us = SECOND,
		.reflector_health_check_interval_us = SECOND,
		.reflector_response_deadline_us = SECOND,
		.reflector_misbehaving_detection_window = 60U,
		.reflector_misbehaving_detection_threshold = 3U,
		.reflector_replacement_interval_us = 60U * MINUTE,
		.reflector_comparison_interval_us = MINUTE,
		.reflector_sum_owd_baselines_delta_threshold_us = 20U * MILLISECOND,
		.reflector_owd_delta_ewma_delta_threshold_us = 10U * MILLISECOND,
		.stall_detection_threshold = 5U,
		.connection_stall_threshold_bps = 10U * KILOBIT,
		.global_ping_response_timeout_us = 10U * SECOND,
		.interface_up_check_interval_us = 10U * SECOND
	};
	(void)strcpy(config->pinger_method, PINGER_METHOD_FPING);
}
