#include "config/config.h"
#include "config/defaults.h"
#include "common/error.h"
#include "latency/latency.h"
#include "constants/cake.h"
#include "constants/mqtt.h"
#include "constants/program.h"
#include "constants/states.h"
#include "constants/units.h"

#include <limits.h>
#include <string.h>

/* A uloop interval: a positive whole number of milliseconds that fits its unsigned argument. */
static bool timer_interval_valid(uint64_t interval_us)
{
	return interval_us > 0U && interval_us % MICROSECONDS_PER_MILLISECOND == 0U &&
	       interval_us / MICROSECONDS_PER_MILLISECOND <= UINT_MAX;
}

/* The time between two reflectors' probes, whole microseconds truncated. */
static uint64_t ping_spacing_us(const struct config *config)
{
	return config->reflector_ping_interval_us / config->no_pingers;
}

static int validate_rate_range(
	const struct config_direction *rates,
	const char *direction,
	char *error,
	size_t error_size
)
{
	uint64_t minimum_bps = rates->minimum_rate_bps;
	uint64_t base_bps = rates->base_rate_bps;
	uint64_t maximum_bps = rates->maximum_rate_bps;

	if (!rates->adjust && minimum_bps == 0U && base_bps == 0U && maximum_bps == 0U)
		return 0;
	if (minimum_bps == 0U || base_bps == 0U || maximum_bps == 0U) {
		return error_set(
			error,
			error_size,
			"%s min/base/max rates are required together",
			direction
		);
	}
	if (minimum_bps > base_bps || base_bps > maximum_bps) {
		return error_set(
			error,
			error_size,
			"%s rates must satisfy minimum <= base <= maximum",
			direction
		);
	}
	/* CAKE holds whole bytes/s. */
	if (minimum_bps % SHAPER_RATE_STEP_BPS != 0U || base_bps % SHAPER_RATE_STEP_BPS != 0U ||
	    maximum_bps % SHAPER_RATE_STEP_BPS != 0U) {
		return error_set(
			error,
			error_size,
			"%s shaper rates must be whole bytes/s (multiples of 8 bit/s)",
			direction
		);
	}
	return 0;
}

static int validate_reflectors(const struct config *config, char *error, size_t error_size)
{
	uint64_t index;
	uint64_t comparison;

	if (config->reflector_count == 0U)
		return error_set(error, error_size, "at least one reflector is required");
	if (config->no_pingers == 0U || config->no_pingers > config->reflector_count) {
		return error_set(
			error,
			error_size,
			"option 'no_pingers' must be between 1 and the reflector count"
		);
	}
	for (index = 0U; index < config->reflector_count; index++) {
		const char *reflector = config->reflectors[index];

		if (!latency_target_valid(reflector))
			return error_set(error, error_size, "invalid reflector '%s'", reflector);
		for (comparison = index + 1U; comparison < config->reflector_count; comparison++)
			if (strcmp(reflector, config->reflectors[comparison]) == 0)
				return error_set(
					error,
					error_size,
					"duplicate reflector '%s'",
					reflector
				);
	}
	return 0;
}

static int validate_pinger(const struct config *config, char *error, size_t error_size)
{
	enum pinger_method method = pinger_method_parse(config->pinger_method);

	if (method == PINGER_METHOD_UNKNOWN) {
		return error_set(
			error,
			error_size,
			"option 'pinger_method' must be 'fping', 'fping-ts', 'irtt', 'native' or 'native-ts'"
		);
	}
	if (method == PINGER_METHOD_IRTT && config->irtt_session_duration_us == 0U) {
		return error_set(
			error,
			error_size,
			"option 'irtt_session_duration_m' must be positive for irtt"
		);
	}
	/* Loading bounds the reflector list; validation bounds no_pingers by it. */
	if (validate_reflectors(config, error, error_size) != 0)
		return -1;
	if (pinger_uses_timestamps(method)) {
		uint64_t index;

		for (index = 0U; index < config->reflector_count; index++) {
			if (strchr(config->reflectors[index], ':') != NULL) {
				return error_set(
					error,
					error_size,
					"pinger_method '%s' uses IPv4-only ICMP timestamps; reflector '%s' is IPv6",
					config->pinger_method,
					config->reflectors[index]
				);
			}
		}
	}
	/* Only a shared echo delay needs attributing to a direction. */
	if (config->tcp_delay_attribution && !pinger_shared_delay(method)) {
		return error_set(
			error,
			error_size,
			"option 'tcp_delay_attribution' needs pinger_method 'fping' or 'native'"
		);
	}
	if (pinger_is_native(method)) {
		struct icmp_address address;

		if (config->ping_extra_args[0] != '\0' || config->ping_prefix_string[0] != '\0')
			return error_set(
				error,
				error_size,
				"native probes do not use ping_extra_args or ping_prefix_string; set pinger_method 'fping' or 'irtt' to use them"
			);
		for (uint64_t index = 0U; index < config->reflector_count; index++)
			if (icmp_address_parse(&address, config->reflectors[index]) != 0)
				return error_set(
					error,
					error_size,
					"native reflector '%s' must be a numeric IP address; use one, or set pinger_method 'fping' for hostnames",
					config->reflectors[index]
				);
	}
	/* Injected timestamps serve only the queue estimate of the attribution. */
	if (config->tcp_ts_request && !config->tcp_delay_attribution) {
		return error_set(
			error,
			error_size,
			"option 'tcp_ts_request' needs tcp_delay_attribution"
		);
	}
	if (ping_spacing_us(config) < MILLISECOND) {
		return error_set(
			error,
			error_size,
			"option 'reflector_ping_interval_s' must provide at least"
			" 1 ms per active reflector"
		);
	}
	return 0;
}

static int validate_detection(const struct config *config, char *error, size_t error_size)
{
	if (!timer_interval_valid(config->monitor_achieved_rates_interval_us)) {
		return error_set(
			error,
			error_size,
			"option 'monitor_achieved_rates_interval_ms' must be a positive whole"
			" number of milliseconds no greater than %u",
			UINT_MAX
		);
	}
	if (config->bufferbloat_detection_window == 0U ||
	    config->bufferbloat_detection_window > UINT_MAX) {
		return error_set(
			error,
			error_size,
			"option 'bufferbloat_detection_window' must be between 1 and %u",
			UINT_MAX
		);
	}
	if (config->bufferbloat_detection_threshold > config->bufferbloat_detection_window) {
		return error_set(
			error,
			error_size,
			"option 'bufferbloat_detection_thr' cannot be greater than"
			" 'bufferbloat_detection_window'"
		);
	}
	if (config->ul_congest_ack_share_ratio_e6 > RATIO_ONE_E6) {
		return error_set(
			error,
			error_size,
			"option 'ul_congest_ack_share' must be between 0 and 1"
		);
	}
	if (config->alpha_baseline_increase_ratio_e6 > RATIO_ONE_E6 ||
	    config->alpha_baseline_decrease_ratio_e6 > RATIO_ONE_E6 ||
	    config->alpha_delta_ewma_ratio_e6 > RATIO_ONE_E6) {
		return error_set(error, error_size, "alpha options must be between 0 and 1");
	}
	return 0;
}

static int validate_reflector_policy(const struct config *config, char *error, size_t error_size)
{
	if (config->reflector_health_check_interval_us == 0U ||
	    config->reflector_response_deadline_us == 0U) {
		return error_set(
			error,
			error_size,
			"reflector health interval and response deadline must be positive"
		);
	}
	if (!timer_interval_valid(config->reflector_health_check_interval_us)) {
		return error_set(
			error,
			error_size,
			"option 'reflector_health_check_interval_s' must be a whole"
			" number of milliseconds no greater than %u",
			UINT_MAX
		);
	}
	if (config->reflector_misbehaving_detection_window == 0U ||
	    config->reflector_misbehaving_detection_window > SIZE_MAX ||
	    config->reflector_misbehaving_detection_threshold == 0U ||
	    config->reflector_misbehaving_detection_threshold >
		    config->reflector_misbehaving_detection_window) {
		return error_set(
			error,
			error_size,
			"reflector offence threshold must be between 1 and its window"
		);
	}
	if (config->stall_detection_threshold == 0U ||
	    config->stall_detection_threshold > UINT64_MAX / ping_spacing_us(config) ||
	    config->global_ping_response_timeout_us == 0U ||
	    config->interface_up_check_interval_us == 0U) {
		return error_set(
			error,
			error_size,
			"stall, global ping timeout and interface retry settings must be"
			" positive and representable"
		);
	}
	return 0;
}

static int validate_monitoring(const struct config *config, char *error, size_t error_size)
{
	const struct config_direction *download = &config->directions[DIRECTION_DOWNLOAD];
	const struct config_direction *upload = &config->directions[DIRECTION_UPLOAD];

	if (config_cpu_stats(config) &&
	    !timer_interval_valid(config->monitor_cpu_usage_interval_us)) {
		return error_set(
			error,
			error_size,
			"CPU monitoring interval must be a positive whole number of milliseconds no greater than %u",
			UINT_MAX
		);
	}
	if (config->log_file_buffer_timeout_us / MICROSECONDS_PER_MILLISECOND > UINT_MAX ||
	    config->reflector_ping_interval_us > UINT64_MAX / 2U) {
		return error_set(error, error_size, "logging or pinger intervals are too large");
	}
	if (config->enable_sleep_function &&
	    (config->connection_active_threshold_bps > download->minimum_rate_bps ||
	     config->connection_active_threshold_bps > upload->minimum_rate_bps)) {
		return error_set(
			error,
			error_size,
			"connection active threshold cannot exceed either minimum shaper rate"
		);
	}
	return 0;
}

/* Enabled, or with rate control configured: the settings it runs with must be complete. */
static bool config_in_use(const struct config *config)
{
	return config->enabled || config_adjusts(config);
}

static int validate_latency_config(const struct config *config, char *error, size_t error_size)
{
	if (!config_in_use(config))
		return 0;
	if (validate_pinger(config, error, error_size) != 0 ||
	    validate_detection(config, error, error_size) != 0 ||
	    validate_reflector_policy(config, error, error_size) != 0)
		return -1;
	return validate_monitoring(config, error, error_size);
}

/* The broker settings an MQTT package connects with. */
static int validate_mqtt_broker(const struct mqtt_settings *mqtt, char *error, size_t error_size)
{
	if (mqtt->host[0] == '\0' || strpbrk(mqtt->host, MQTT_INVALID_HOST_CHARACTERS) != NULL)
		return error_set(
			error,
			error_size,
			"option 'mqtt_host' must be a hostname or IP address"
		);
	if (mqtt->port == 0U || mqtt->port > UINT16_MAX)
		return error_set(
			error,
			error_size,
			"option 'mqtt_port' must be between 1 and 65535"
		);
	if (mqtt->topic[0] == '\0' || strpbrk(mqtt->topic, MQTT_TOPIC_WILDCARDS) != NULL)
		return error_set(
			error,
			error_size,
			"option 'mqtt_topic' must be nonempty and contain no + or #"
		);
	if (mqtt->password[0] != '\0' && mqtt->username[0] == '\0')
		return error_set(error, error_size, "option 'mqtt_password' needs mqtt_username");
	return 0;
}

static int validate_mqtt(const struct mqtt_settings *mqtt, char *error, size_t error_size)
{
	if (!mqtt->enabled)
		return 0;
	/*
	 * The cake-adapt package sends nothing: its sender's start fails, main() warns and the
	 * instance runs, so the broker settings are not checked. TLS asked for is refused in every
	 * package without it, never replaced by plain traffic.
	 */
	if (MQTT_SUPPORTED && validate_mqtt_broker(mqtt, error, error_size) != 0)
		return -1;
	if (mqtt->tls) {
#if !MQTT_TLS_SUPPORTED
		return error_set(
			error,
			error_size,
			"option 'mqtt_tls' requires the cake-adapt-mqtt-ssl package"
		);
#else
		if (mqtt->ca_file[0] != '/')
			return error_set(
				error,
				error_size,
				"option 'mqtt_ca_file' must be an absolute path"
			);
#endif
	}
	return 0;
}

int config_validate(const struct config *config, char *error, size_t error_size)
{
	if (validate_mqtt(&config->mqtt, error, error_size) != 0)
		return -1;
	if (config_in_use(config) && config->interface[0] == '\0') {
		return error_set(
			error,
			error_size,
			"set either option 'interface' or both options 'ul_if' and"
			" 'dl_if' when cake-adapt is enabled or rate adjustment is"
			" configured"
		);
	}
	for (size_t direction = 0U; direction < DIRECTION_COUNT; direction++)
		if (validate_rate_range(
			    &config->directions[direction],
			    direction_name((enum direction)direction),
			    error,
			    error_size
		    ) != 0)
			return -1;
	return validate_latency_config(config, error, error_size);
}
