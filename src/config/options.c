#include "config/options.h"
#include "config/config.h"
#include "common/utils.h"
#include "constants/options.h"
#include "constants/units.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#define CONFIG_OFFSET(member) offsetof(struct config, member)
#define CONFIG_SIZE(member) sizeof(((struct config *)0)->member)
#define BOOLEAN_OPTION(name, member) { name, TYPE_BOOLEAN, CONFIG_OFFSET(member), 0U, 0U }
#define STRING_OPTION(name, member) \
	{ name, TYPE_STRING, CONFIG_OFFSET(member), CONFIG_SIZE(member), 0U }
#define SCALED_OPTION(name, member, scale) { name, TYPE_SCALED, CONFIG_OFFSET(member), 0U, scale }
/* A ratio with up to six decimal places, stored per million. */
#define RATIO_OPTION(name, member) SCALED_OPTION(name, member, RATIO_ONE_E6)

/* Booleans, then strings, then decimals: the order -L lists and loading applies. */
static const struct option_binding options[] = {
	BOOLEAN_OPTION(OPTION_ENABLED, enabled),
	BOOLEAN_OPTION(OPTION_ADJUST_DOWNLOAD, download.adjust),
	BOOLEAN_OPTION(OPTION_ADJUST_UPLOAD, upload.adjust),
	BOOLEAN_OPTION(OPTION_OUTPUT_PROCESSING_STATS, output_processing_stats),
	BOOLEAN_OPTION(OPTION_OUTPUT_LOAD_STATS, output_load_stats),
	BOOLEAN_OPTION(OPTION_OUTPUT_REFLECTOR_STATS, output_reflector_stats),
	BOOLEAN_OPTION(OPTION_OUTPUT_SUMMARY_STATS, output_summary_stats),
	BOOLEAN_OPTION(OPTION_OUTPUT_CAKE_CHANGES, output_cake_changes),
	BOOLEAN_OPTION(OPTION_OUTPUT_CPU_STATS, output_cpu_stats),
	BOOLEAN_OPTION(OPTION_OUTPUT_CPU_RAW_STATS, output_cpu_raw_stats),
	BOOLEAN_OPTION(OPTION_OUTPUT_MEMORY_STATS, output_memory_stats),
	BOOLEAN_OPTION(OPTION_DEBUG, debug),
	BOOLEAN_OPTION(OPTION_LOG_DEBUG_TO_SYSLOG, log_debug_messages_to_syslog),
	BOOLEAN_OPTION(OPTION_LOG_TO_FILE, log_to_file),
	BOOLEAN_OPTION(OPTION_MQTT_ENABLED, mqtt.enabled),
	BOOLEAN_OPTION(OPTION_MQTT_TLS, mqtt.tls),
	BOOLEAN_OPTION(OPTION_RANDOMIZE_REFLECTORS, randomize_reflectors),
	BOOLEAN_OPTION(OPTION_RETAIN_REFLECTOR_STATS, retain_reflector_stats),
	BOOLEAN_OPTION(OPTION_ENABLE_SLEEP_FUNCTION, enable_sleep_function),
	BOOLEAN_OPTION(OPTION_MIN_SHAPER_RATES_ENFORCEMENT, minimum_shaper_rates_enforcement),
	BOOLEAN_OPTION(OPTION_LOG_FILE_EXPORT_COMPRESS, log_file_export_compress),
	BOOLEAN_OPTION(OPTION_TCP_DELAY_ATTRIBUTION, tcp_delay_attribution),
	BOOLEAN_OPTION(OPTION_TCP_TS_REQUEST, tcp_ts_request),
	STRING_OPTION(OPTION_INTERFACE, interface),
	STRING_OPTION(OPTION_UPLOAD_INTERFACE, ul_if),
	STRING_OPTION(OPTION_DOWNLOAD_INTERFACE, dl_if),
	STRING_OPTION(OPTION_CONFIG_FILE, config_file),
	STRING_OPTION(OPTION_LOG_FILE_PATH_OVERRIDE, log_file_path_override),
	STRING_OPTION(OPTION_PINGER_METHOD, pinger_method),
	STRING_OPTION(OPTION_PING_EXTRA_ARGS, ping_extra_args),
	STRING_OPTION(OPTION_PING_PREFIX_STRING, ping_prefix_string),
	STRING_OPTION(OPTION_MQTT_HOST, mqtt.host),
	STRING_OPTION(OPTION_MQTT_TOPIC, mqtt.topic),
	STRING_OPTION(OPTION_MQTT_USERNAME, mqtt.username),
	STRING_OPTION(OPTION_MQTT_PASSWORD, mqtt.password),
	STRING_OPTION(OPTION_MQTT_CA_FILE, mqtt.ca_file),
	SCALED_OPTION(OPTION_MQTT_PORT, mqtt.port, 1U),
	SCALED_OPTION(OPTION_LOG_FILE_MAX_TIME, log_file_max_time_us, MINUTE),
	SCALED_OPTION(OPTION_LOG_FILE_MAX_SIZE, log_file_max_size_bytes, KILOBYTE),
	SCALED_OPTION(OPTION_NO_PINGERS, no_pingers, 1U),
	SCALED_OPTION(OPTION_REFLECTOR_PING_INTERVAL, reflector_ping_interval_us, SECOND),
	SCALED_OPTION(OPTION_MIN_DOWNLOAD_RATE, download.minimum_rate_bps, KILOBIT),
	SCALED_OPTION(OPTION_BASE_DOWNLOAD_RATE, download.base_rate_bps, KILOBIT),
	SCALED_OPTION(OPTION_MAX_DOWNLOAD_RATE, download.maximum_rate_bps, KILOBIT),
	SCALED_OPTION(OPTION_MIN_UPLOAD_RATE, upload.minimum_rate_bps, KILOBIT),
	SCALED_OPTION(OPTION_BASE_UPLOAD_RATE, upload.base_rate_bps, KILOBIT),
	SCALED_OPTION(OPTION_MAX_UPLOAD_RATE, upload.maximum_rate_bps, KILOBIT),
	SCALED_OPTION(OPTION_CONNECTION_ACTIVE_THRESHOLD, connection_active_threshold_bps, KILOBIT),
	SCALED_OPTION(OPTION_CONNECTION_STALL_THRESHOLD, connection_stall_threshold_bps, KILOBIT),
	SCALED_OPTION(
		OPTION_DOWNLOAD_AVG_ADJUST_UP,
		download.average_owd_delta_maximum_adjust_up_us,
		MILLISECOND
	),
	SCALED_OPTION(
		OPTION_UPLOAD_AVG_ADJUST_UP,
		upload.average_owd_delta_maximum_adjust_up_us,
		MILLISECOND
	),
	SCALED_OPTION(
		OPTION_DOWNLOAD_DELAY_THRESHOLD,
		download.owd_delta_delay_threshold_us,
		MILLISECOND
	),
	SCALED_OPTION(
		OPTION_UPLOAD_DELAY_THRESHOLD,
		upload.owd_delta_delay_threshold_us,
		MILLISECOND
	),
	SCALED_OPTION(
		OPTION_DOWNLOAD_AVG_ADJUST_DOWN,
		download.average_owd_delta_maximum_adjust_down_us,
		MILLISECOND
	),
	SCALED_OPTION(
		OPTION_UPLOAD_AVG_ADJUST_DOWN,
		upload.average_owd_delta_maximum_adjust_down_us,
		MILLISECOND
	),
	SCALED_OPTION(OPTION_SUSTAINED_IDLE_SLEEP, sustained_idle_sleep_threshold_us, SECOND),
	SCALED_OPTION(OPTION_LOG_FILE_BUFFER_TIMEOUT, log_file_buffer_timeout_us, MILLISECOND),
	SCALED_OPTION(OPTION_IRTT_SESSION_DURATION, irtt_session_duration_us, MINUTE),
	SCALED_OPTION(
		OPTION_TRAFFIC_MONITOR_INTERVAL,
		monitor_achieved_rates_interval_us,
		MILLISECOND
	),
	SCALED_OPTION(OPTION_CPU_MONITOR_INTERVAL, monitor_cpu_usage_interval_us, MILLISECOND),
	SCALED_OPTION(OPTION_BUFFERBLOAT_WINDOW, bufferbloat_detection_window, 1U),
	SCALED_OPTION(OPTION_BUFFERBLOAT_THRESHOLD, bufferbloat_detection_threshold, 1U),
	RATIO_OPTION(OPTION_ALPHA_BASELINE_INCREASE, alpha_baseline_increase_ratio_e6),
	RATIO_OPTION(OPTION_ALPHA_BASELINE_DECREASE, alpha_baseline_decrease_ratio_e6),
	RATIO_OPTION(OPTION_ALPHA_DELTA_EWMA, alpha_delta_ewma_ratio_e6),
	RATIO_OPTION(
		OPTION_RATE_MIN_DOWN_BUFFERBLOAT,
		shaper_rate_minimum_adjust_down_bufferbloat_ratio_e6
	),
	RATIO_OPTION(
		OPTION_RATE_MAX_DOWN_BUFFERBLOAT,
		shaper_rate_maximum_adjust_down_bufferbloat_ratio_e6
	),
	RATIO_OPTION(OPTION_RATE_MIN_UP_HIGH_LOAD, shaper_rate_minimum_adjust_up_load_high_ratio_e6),
	RATIO_OPTION(OPTION_RATE_MAX_UP_HIGH_LOAD, shaper_rate_maximum_adjust_up_load_high_ratio_e6),
	RATIO_OPTION(OPTION_RATE_DOWN_LOW_LOAD, shaper_rate_adjust_down_load_low_ratio_e6),
	RATIO_OPTION(OPTION_RATE_UP_LOW_LOAD, shaper_rate_adjust_up_load_low_ratio_e6),
	RATIO_OPTION(OPTION_HIGH_LOAD_THRESHOLD, high_load_threshold_ratio_e6),
	RATIO_OPTION(OPTION_UL_CONGEST_ACK_SHARE, ul_congest_ack_share_ratio_e6),
	SCALED_OPTION(OPTION_BUFFERBLOAT_REFRACTORY, bufferbloat_refractory_period_us, MILLISECOND),
	SCALED_OPTION(OPTION_DECAY_REFRACTORY, decay_refractory_period_us, MILLISECOND),
	SCALED_OPTION(OPTION_REFLECTOR_HEALTH_INTERVAL, reflector_health_check_interval_us, SECOND),
	SCALED_OPTION(OPTION_REFLECTOR_RESPONSE_DEADLINE, reflector_response_deadline_us, SECOND),
	SCALED_OPTION(
		OPTION_REFLECTOR_MISBEHAVING_WINDOW,
		reflector_misbehaving_detection_window,
		1U
	),
	SCALED_OPTION(
		OPTION_REFLECTOR_MISBEHAVING_THRESHOLD,
		reflector_misbehaving_detection_threshold,
		1U
	),
	SCALED_OPTION(
		OPTION_REFLECTOR_REPLACEMENT_INTERVAL,
		reflector_replacement_interval_us,
		MINUTE
	),
	SCALED_OPTION(OPTION_REFLECTOR_COMPARISON_INTERVAL, reflector_comparison_interval_us, MINUTE),
	SCALED_OPTION(
		OPTION_REFLECTOR_BASELINE_DELTA,
		reflector_sum_owd_baselines_delta_threshold_us,
		MILLISECOND
	),
	SCALED_OPTION(
		OPTION_REFLECTOR_EWMA_DELTA,
		reflector_owd_delta_ewma_delta_threshold_us,
		MILLISECOND
	),
	SCALED_OPTION(OPTION_STALL_DETECTION_THRESHOLD, stall_detection_threshold, 1U),
	SCALED_OPTION(OPTION_GLOBAL_PING_TIMEOUT, global_ping_response_timeout_us, SECOND),
	SCALED_OPTION(OPTION_INTERFACE_UP_INTERVAL, interface_up_check_interval_us, SECOND),
};

const struct option_binding *config_option(size_t index)
{
	return &options[index];
}

size_t config_option_count(void)
{
	return ARRAY_SIZE(options);
}

const char *config_option_name(size_t index)
{
	return index < ARRAY_SIZE(options) ? options[index].name : NULL;
}

/* value / scale as the shortest decimal with at most six places, as UCI holds it. */
static void format_scaled(uint64_t value, uint64_t scale, char *text, size_t text_size)
{
	uint64_t whole = value / scale;
	uint64_t millionths = rounded_divide(value % scale * RATIO_ONE_E6, scale);
	int length;

	if (millionths == RATIO_ONE_E6) {
		whole++;
		millionths = 0U;
	}
	if (millionths == 0U) {
		(void)snprintf(text, text_size, "%" PRIu64, whole);
		return;
	}
	length = snprintf(text, text_size, "%" PRIu64 ".%06" PRIu64, whole, millionths);
	while (length > 0 && (size_t)length < text_size && text[length - 1] == '0')
		text[--length] = '\0';
}

int config_option_value(const struct config *config, size_t index, char *text, size_t text_size)
{
	const struct option_binding *option;
	const char *member;

	if (index >= ARRAY_SIZE(options))
		return -1;
	option = &options[index];
	member = (const char *)config + option->offset;
	switch (option->type) {
	case TYPE_BOOLEAN:
		(void)snprintf(
			text,
			text_size,
			"%s",
			*(const bool *)member ? BOOLEAN_TRUE_NUMERIC : BOOLEAN_FALSE_NUMERIC
		);
		break;
	case TYPE_STRING:
		(void)snprintf(text, text_size, "%s", member);
		break;
	case TYPE_SCALED:
		format_scaled(*(const uint64_t *)member, option->scale, text, text_size);
		break;
	}
	return 0;
}
