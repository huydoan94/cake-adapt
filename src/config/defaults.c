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
		.ul_congest_ack_share_per_million = 0U,
		.log_file_max_time_minutes = 10U,
		.log_file_max_size_kilobytes = 2000U,
		.no_pingers = 6U,
		.reflector_ping_interval_microseconds = 300U * MILLISECOND,
		.download.average_owd_delta_maximum_adjust_up_microseconds = 10U * MILLISECOND,
		.upload.average_owd_delta_maximum_adjust_up_microseconds = 10U * MILLISECOND,
		.download.owd_delta_delay_threshold_microseconds = 30U * MILLISECOND,
		.upload.owd_delta_delay_threshold_microseconds = 30U * MILLISECOND,
		.download.average_owd_delta_maximum_adjust_down_microseconds = 60U * MILLISECOND,
		.upload.average_owd_delta_maximum_adjust_down_microseconds = 60U * MILLISECOND,
		.download.minimum_rate_bits_per_second = 5U * MEGABIT,
		.download.base_rate_bits_per_second = 20U * MEGABIT,
		.download.maximum_rate_bits_per_second = 80U * MEGABIT,
		.upload.minimum_rate_bits_per_second = 5U * MEGABIT,
		.upload.base_rate_bits_per_second = 20U * MEGABIT,
		.upload.maximum_rate_bits_per_second = 35U * MEGABIT,
		.connection_active_threshold_bits_per_second = 2U * MEGABIT,
		.sustained_idle_sleep_threshold_microseconds = MINUTE,
		.log_file_buffer_timeout_microseconds = 500U * MILLISECOND,
		.irtt_session_duration_minutes = 10U,
		.monitor_achieved_rates_interval_microseconds = 200U * MILLISECOND,
		.monitor_cpu_usage_interval_microseconds = 2U * SECOND,
		.bufferbloat_detection_window = 6U,
		.bufferbloat_detection_threshold = 3U,
		.alpha_baseline_increase_per_million = 1U * MILLION / PERCENT / 10U,
		.alpha_baseline_decrease_per_million = 90U * MILLION / PERCENT,
		.alpha_delta_ewma_per_million = 95U * MILLION / PERCENT / 10U,
		.shaper_rate_minimum_adjust_down_bufferbloat_per_million = 99U * MILLION / PERCENT,
		.shaper_rate_maximum_adjust_down_bufferbloat_per_million = 75U * MILLION / PERCENT,
		.shaper_rate_minimum_adjust_up_load_high_per_million = 100U * MILLION / PERCENT,
		.shaper_rate_maximum_adjust_up_load_high_per_million = 104U * MILLION / PERCENT,
		.shaper_rate_adjust_down_load_low_per_million = 99U * MILLION / PERCENT,
		.shaper_rate_adjust_up_load_low_per_million = 101U * MILLION / PERCENT,
		.high_load_threshold_per_million = 75U * MILLION / PERCENT,
		.bufferbloat_refractory_period_microseconds = 300U * MILLISECOND,
		.decay_refractory_period_microseconds = SECOND,
		.reflector_health_check_interval_microseconds = SECOND,
		.reflector_response_deadline_microseconds = SECOND,
		.reflector_misbehaving_detection_window = 60U,
		.reflector_misbehaving_detection_threshold = 3U,
		.reflector_replacement_interval_minutes = 60U,
		.reflector_comparison_interval_minutes = 1U,
		.reflector_sum_owd_baselines_delta_threshold_microseconds = 20U * MILLISECOND,
		.reflector_owd_delta_ewma_delta_threshold_microseconds = 10U * MILLISECOND,
		.stall_detection_threshold = 5U,
		.connection_stall_threshold_bits_per_second = 10U * KILOBIT,
		.global_ping_response_timeout_microseconds = 10U * SECOND,
		.interface_up_check_interval_microseconds = 10U * SECOND
	};
	(void)strcpy(config->pinger_method, PINGER_METHOD_FPING);
}
