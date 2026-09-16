#ifndef SQM_MON_CONFIG_H
#define SQM_MON_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <net/if.h>

#define CONFIG_LATENCY_TARGET_SIZE 256U
#define CONFIG_STRING_SIZE 512U
#define CONFIG_REFLECTOR_SIZE 256U
#define CONFIG_MAX_REFLECTORS 64U

struct sqm_mon_config {
    bool enabled;
    bool adjust_download;
    bool adjust_upload;
    bool output_processing_stats;
    bool output_load_stats;
    bool output_reflector_stats;
    bool output_summary_stats;
    bool output_cake_changes;
    bool output_cpu_stats;
    bool output_cpu_raw_stats;
    bool debug;
    bool log_debug_messages_to_syslog;
    bool log_to_file;
    bool randomize_reflectors;
    bool retain_reflector_stats;
    bool enable_sleep_function;
    bool minimum_shaper_rates_enforcement;
    bool log_file_export_compress;
    char interface[IF_NAMESIZE];
    char ingress_interface[IF_NAMESIZE];
    char latency_target[CONFIG_LATENCY_TARGET_SIZE];
    char log_file_path_override[CONFIG_STRING_SIZE];
    char pinger_method[CONFIG_STRING_SIZE];
    char reflectors[CONFIG_MAX_REFLECTORS][CONFIG_REFLECTOR_SIZE];
    char reflectors_url[CONFIG_STRING_SIZE];
    char ping_extra_args[CONFIG_STRING_SIZE];
    char ping_prefix_string[CONFIG_STRING_SIZE];
    uint64_t reflector_count;
    uint64_t log_file_max_time_minutes;
    uint64_t log_file_max_size_kilobytes;
    uint64_t reflectors_url_skip_lines;
    uint64_t no_pingers;
    uint64_t reflector_ping_interval_microseconds;
    uint64_t download_average_owd_delta_maximum_adjust_up_microseconds;
    uint64_t upload_average_owd_delta_maximum_adjust_up_microseconds;
    uint64_t download_owd_delta_delay_threshold_microseconds;
    uint64_t upload_owd_delta_delay_threshold_microseconds;
    uint64_t download_average_owd_delta_maximum_adjust_down_microseconds;
    uint64_t upload_average_owd_delta_maximum_adjust_down_microseconds;
    uint64_t minimum_download_rate_bits_per_second;
    uint64_t base_download_rate_bits_per_second;
    uint64_t maximum_download_rate_bits_per_second;
    uint64_t minimum_upload_rate_bits_per_second;
    uint64_t base_upload_rate_bits_per_second;
    uint64_t maximum_upload_rate_bits_per_second;
    uint64_t connection_active_threshold_bits_per_second;
    uint64_t sustained_idle_sleep_threshold_microseconds;
    uint64_t startup_wait_microseconds;
    uint64_t log_file_buffer_timeout_microseconds;
    uint64_t irtt_session_duration_minutes;
    uint64_t monitor_achieved_rates_interval_microseconds;
    uint64_t monitor_cpu_usage_interval_microseconds;
    uint64_t bufferbloat_detection_window;
    uint64_t bufferbloat_detection_threshold;
    uint64_t alpha_baseline_increase_per_million;
    uint64_t alpha_baseline_decrease_per_million;
    uint64_t alpha_delta_ewma_per_million;
    uint64_t shaper_rate_minimum_adjust_down_bufferbloat_per_million;
    uint64_t shaper_rate_maximum_adjust_down_bufferbloat_per_million;
    uint64_t shaper_rate_minimum_adjust_up_load_high_per_million;
    uint64_t shaper_rate_maximum_adjust_up_load_high_per_million;
    uint64_t shaper_rate_adjust_down_load_low_per_million;
    uint64_t shaper_rate_adjust_up_load_low_per_million;
    uint64_t high_load_threshold_per_million;
    uint64_t bufferbloat_refractory_period_microseconds;
    uint64_t decay_refractory_period_microseconds;
    uint64_t reflector_health_check_interval_microseconds;
    uint64_t reflector_response_deadline_microseconds;
    uint64_t reflector_misbehaving_detection_window;
    uint64_t reflector_misbehaving_detection_threshold;
    uint64_t reflector_replacement_interval_minutes;
    uint64_t reflector_comparison_interval_minutes;
    uint64_t reflector_sum_owd_baselines_delta_threshold_microseconds;
    uint64_t reflector_owd_delta_ewma_delta_threshold_microseconds;
    uint64_t stall_detection_threshold;
    uint64_t connection_stall_threshold_bits_per_second;
    uint64_t global_ping_response_timeout_microseconds;
    uint64_t interface_up_check_interval_microseconds;
};

int config_load(
    struct sqm_mon_config *config,
    const char *config_directory,
    char *error,
    size_t error_size
);

int config_load_remote_reflectors(
    struct sqm_mon_config *config,
    char *error,
    size_t error_size
);

#endif
