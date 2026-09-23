#include "defaults.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct config config;

    defaults_apply(&config);

    assert(!config.enabled);
    assert(!config.adjust_download);
    assert(!config.adjust_upload);
    assert(config.debug);
    assert(config.log_to_file);
    assert(config.randomize_reflectors);
    assert(config.retain_reflector_stats);
    assert(config.enable_sleep_function);
    assert(config.log_file_export_compress);
    assert(strcmp(config.pinger_method, "fping") == 0);
    assert(config.log_file_max_time_minutes == 10U);
    assert(config.log_file_max_size_kilobytes == 2000U);
    assert(config.no_pingers == 6U);
    assert(config.reflector_ping_interval_microseconds == 300000U);
    assert(config.minimum_download_rate_bits_per_second == 5000000U);
    assert(config.base_download_rate_bits_per_second == 20000000U);
    assert(config.maximum_download_rate_bits_per_second == 80000000U);
    assert(config.minimum_upload_rate_bits_per_second == 5000000U);
    assert(config.base_upload_rate_bits_per_second == 20000000U);
    assert(config.maximum_upload_rate_bits_per_second == 35000000U);
    assert(config.connection_active_threshold_bits_per_second == 2000000U);
    assert(config.monitor_achieved_rates_interval_microseconds == 200000U);
    assert(config.bufferbloat_detection_window == 6U);
    assert(config.bufferbloat_detection_threshold == 3U);
    assert(config.global_ping_response_timeout_microseconds == 10000000U);
    assert(config.interface_up_check_interval_microseconds == 10000000U);

    (void)puts("default configuration tests passed");
    return 0;
}
