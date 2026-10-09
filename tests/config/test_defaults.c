#include "config/defaults.h"
#include "config/config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	struct config config;

	defaults_apply(&config);

	assert(!config.enabled);
	assert(!config.download.adjust);
	assert(!config.upload.adjust);
	assert(config.debug);
	assert(config.log_to_file);
	assert(config.randomize_reflectors);
	assert(config.retain_reflector_stats);
	assert(config.enable_sleep_function);
	assert(config.log_file_export_compress);
	assert(strcmp(config.pinger_method, "fping") == 0);
	assert(config.log_file_max_time_us == 600000000U);
	assert(config.log_file_max_size_bytes == 2048000U);
	assert(config.no_pingers == 6U);
	assert(config.reflector_ping_interval_us == 300000U);
	assert(config.download.minimum_rate_bps == 5000000U);
	assert(config.download.base_rate_bps == 20000000U);
	assert(config.download.maximum_rate_bps == 80000000U);
	assert(config.upload.minimum_rate_bps == 5000000U);
	assert(config.upload.base_rate_bps == 20000000U);
	assert(config.upload.maximum_rate_bps == 35000000U);
	assert(config.connection_active_threshold_bps == 2000000U);
	assert(config.monitor_achieved_rates_interval_us == 200000U);
	assert(!config.tcp_delay_attribution);
	assert(!config.tcp_timestamp_inject);
	assert(config.tcp_ts_stall_window_us == 40000000U);
	assert(config.ul_congest_ack_share_ratio_e6 == 0U);
	assert(config.bufferbloat_detection_window == 6U);
	assert(config.bufferbloat_detection_threshold == 3U);
	assert(config.global_ping_response_timeout_us == 10000000U);
	assert(config.interface_up_check_interval_us == 10000000U);

	(void)puts("default configuration tests passed");
	return 0;
}
