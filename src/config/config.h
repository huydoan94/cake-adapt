#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

#include "logging/mqtt.h"
#include "constants/options.h"
#include "constants/units.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <net/if.h>

/* One direction's shaper rates and the delay thresholds that adjust them. */
struct config_direction {
	bool adjust;
	uint64_t minimum_rate_bps;
	uint64_t base_rate_bps;
	uint64_t maximum_rate_bps;
	uint64_t average_owd_delta_maximum_adjust_up_us;
	uint64_t owd_delta_delay_threshold_us;
	uint64_t average_owd_delta_maximum_adjust_down_us;
};

struct config {
	struct mqtt_settings mqtt;
	bool enabled;
	bool output_processing_stats;
	bool output_load_stats;
	bool output_reflector_stats;
	bool output_summary_stats;
	bool output_cake_changes;
	bool output_cpu_stats;
	bool output_cpu_raw_stats;
	/* Not in cake-autorate: the daemon's own memory use. */
	bool output_memory_stats;
	bool debug;
	bool log_debug_messages_to_syslog;
	bool log_to_file;
	bool randomize_reflectors;
	bool retain_reflector_stats;
	bool enable_sleep_function;
	bool minimum_shaper_rates_enforcement;
	bool log_file_export_compress;
	/* Attribute an echo pinger's shared delay with per-direction TCP-timestamp queues. */
	bool tcp_delay_attribution;
	/* Experimental: add TCP timestamps to SYNs without them, so they can be measured. */
	bool tcp_ts_request;
	bool interface_overridden;
	char interface[IF_NAMESIZE];
	char ingress_interface[IF_NAMESIZE];
	char ul_if[IF_NAMESIZE];
	char dl_if[IF_NAMESIZE];
	char config_file[CONFIG_STRING_SIZE];
	char log_file_path_override[CONFIG_STRING_SIZE];
	char pinger_method[CONFIG_STRING_SIZE];
	char reflectors[CONFIG_MAX_REFLECTORS][CONFIG_REFLECTOR_SIZE];
	char ping_extra_args[CONFIG_STRING_SIZE];
	char ping_prefix_string[CONFIG_STRING_SIZE];
	uint64_t reflector_count;
	uint64_t log_file_max_time_us;
	uint64_t log_file_max_size_bytes;
	uint64_t no_pingers;
	uint64_t reflector_ping_interval_us;
	struct config_direction download;
	struct config_direction upload;
	uint64_t connection_active_threshold_bps;
	uint64_t sustained_idle_sleep_threshold_us;
	uint64_t log_file_buffer_timeout_us;
	uint64_t irtt_session_duration_us;
	uint64_t monitor_achieved_rates_interval_us;
	uint64_t monitor_cpu_usage_interval_us;
	uint64_t bufferbloat_detection_window;
	uint64_t bufferbloat_detection_threshold;
	/* Ratios are integers per million: RATIO_ONE_E6 is 100%. */
	uint64_t alpha_baseline_increase_ratio_e6;
	uint64_t alpha_baseline_decrease_ratio_e6;
	uint64_t alpha_delta_ewma_ratio_e6;
	uint64_t shaper_rate_minimum_adjust_down_bufferbloat_ratio_e6;
	uint64_t shaper_rate_maximum_adjust_down_bufferbloat_ratio_e6;
	uint64_t shaper_rate_minimum_adjust_up_load_high_ratio_e6;
	uint64_t shaper_rate_maximum_adjust_up_load_high_ratio_e6;
	uint64_t shaper_rate_adjust_down_load_low_ratio_e6;
	uint64_t shaper_rate_adjust_up_load_low_ratio_e6;
	uint64_t high_load_threshold_ratio_e6;
	/* cake-adapt only: ACKs' minimum share of upload; zero disables the hold. */
	uint64_t ul_congest_ack_share_ratio_e6;
	uint64_t bufferbloat_refractory_period_us;
	uint64_t decay_refractory_period_us;
	uint64_t reflector_health_check_interval_us;
	uint64_t reflector_response_deadline_us;
	uint64_t reflector_misbehaving_detection_window;
	uint64_t reflector_misbehaving_detection_threshold;
	uint64_t reflector_replacement_interval_us;
	uint64_t reflector_comparison_interval_us;
	uint64_t reflector_sum_owd_baselines_delta_threshold_us;
	uint64_t reflector_owd_delta_ewma_delta_threshold_us;
	uint64_t stall_detection_threshold;
	uint64_t connection_stall_threshold_bps;
	uint64_t global_ping_response_timeout_us;
	uint64_t interface_up_check_interval_us;
};

/* Rate control is configured for either direction. */
static inline bool config_adjusts(const struct config *config)
{
	const struct config_direction *download = &config->download;
	const struct config_direction *upload = &config->upload;

	return download->adjust || upload->adjust;
}

/* Either CPU record is asked for, so the CPU timer runs. */
static inline bool config_cpu_stats(const struct config *config)
{
	return config->output_cpu_stats || config->output_cpu_raw_stats;
}

size_t config_option_count(void);
const char *config_option_name(size_t index);
/*
 * Option index's value in config, as /etc/config/cake-adapt writes it: 0 or 1,
 * the string, or a decimal in the option's unit (seconds, kbit/s, a ratio).
 */
int config_option_value(const struct config *config, size_t index, char *text, size_t text_size);

/* Checks a loaded configuration for consistency; config_load() calls it. */
int config_validate(const struct config *config, char *error, size_t error_size);

struct stat;

/*
 * Refuses ping_prefix_string, ping_extra_args and log_file_path_override when
 * the configuration file, whose STATUS is given, is SHARED_PATH:
 * /etc/config/cake-adapt, which LuCI can change. The service takes them only
 * from the root-only /etc/cake-adapt/exec, into the runtime file it builds;
 * this keeps a daemon started by hand from running a program, with arguments,
 * or writing where a LuCI user chose.
 */
int config_check_shared_file(
	const struct config *config,
	const struct stat *status,
	const char *shared_path,
	char *error,
	size_t error_size
);

/*
 * Loads SECTION_NAME from CONFIG_PATH, the absolute path of a file in UCI
 * syntax (libuci loads a name that starts with '/' as that file).
 */
int config_load(
	struct config *config,
	const char *config_path,
	const char *section_name,
	char *error,
	size_t error_size
);

#endif
