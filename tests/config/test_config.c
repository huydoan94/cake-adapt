#include "config/config.c"
#include "config/validate.c"
#include "config/options.c"
#include "common/utils.h"
#include "constants/options.h"
#include "constants/program.h"
#include "constants/units.h"

#include <assert.h>
#include <unistd.h>

/* Use real libuci inline accessors with a controlled single-option lookup. */
static struct uci_option *lookup_option;

int uci_lookup_next(
	struct uci_context *context,
	struct uci_element **element,
	struct uci_list *list,
	const char *name
)
{
	struct uci_element *option_element;

	(void)context;
	(void)list;
	if (lookup_option == NULL)
		return UCI_ERR_NOTFOUND;
	option_element = &lookup_option->e;
	if (strcmp(option_element->name, name) != 0)
		return UCI_ERR_NOTFOUND;
	*element = option_element;
	return UCI_OK;
}

/* Parses value as an option named "test" stored multiplied by scale. */
static int
parse_decimal(const char *value, uint64_t scale, uint64_t *result, char *error, size_t error_size)
{
	const struct loader loader = { .error = error, .error_size = error_size };
	const struct option_binding option = { .name = "test",
					       .type = TYPE_SCALED,
					       .scale = scale };

	return parse_scaled_decimal(&loader, &option, value, result);
}

static void test_scalar_option_types(void)
{
	struct uci_section section = { 0 };
	struct uci_option option = {
		.e = { .type = UCI_TYPE_OPTION, .name = "enabled" },
		.type = UCI_TYPE_LIST,
	};
	struct config config = { .no_pingers = 7U, .interface = "default" };
	char error[128];
	const struct loader loader = {
		.section = &section,
		.config = &config,
		.error = error,
		.error_size = sizeof(error),
	};

	lookup_option = &option;
	assert(load_options(&loader) == -1);
	assert(strstr(error, "not a list") != NULL);
	assert(!config.enabled);
	option.e.name = "no_pingers";
	assert(load_options(&loader) == -1);
	assert(config.no_pingers == 7U);
	option.e.name = "interface";
	assert(load_options(&loader) == -1);
	assert(strcmp(config.interface, "default") == 0);

	lookup_option = NULL;
	assert(load_options(&loader) == 0);
	assert(load_options(&loader) == 0);
	assert(load_options(&loader) == 0);
	assert(!config.enabled);
	assert(config.no_pingers == 7U);
	assert(strcmp(config.interface, "default") == 0);

	lookup_option = &option;
	option.type = UCI_TYPE_STRING;
	option.v.string = "1";
	option.e.name = "enabled";
	assert(load_options(&loader) == 0);
	assert(config.enabled);
	option.e.name = "no_pingers";
	assert(load_options(&loader) == 0);
	assert(config.no_pingers == 1U);
	option.e.name = "interface";
	assert(load_options(&loader) == 0);
	assert(strcmp(config.interface, "1") == 0);
	option.e.name = "mqtt_tls";
	assert(load_options(&loader) == 0);
	assert(config.mqtt.tls);
	option.e.name = "mqtt_ca_file";
	option.v.string = "/tmp/fixture-ca.pem";
	assert(load_options(&loader) == 0);
	assert(strcmp(config.mqtt.ca_file, "/tmp/fixture-ca.pem") == 0);
	lookup_option = NULL;
}

static void test_supported_option_names(void)
{
	bool found_adjust_download = false;
	bool found_download_interface = false;
	bool found_ping_arguments = false;
	bool found_ul_congest_ack_share = false;
	bool found_upload_interface = false;
	size_t index;

	for (index = 0U; index < config_option_count(); index++) {
		const char *name = config_option_name(index);

		assert(name != NULL);
		found_adjust_download = found_adjust_download ||
					strcmp(name, "adjust_dl_shaper_rate") == 0;
		found_download_interface = found_download_interface || strcmp(name, "dl_if") == 0;
		found_ping_arguments = found_ping_arguments || strcmp(name, "ping_extra_args") == 0;
		found_ul_congest_ack_share = found_ul_congest_ack_share ||
					     strcmp(name, "ul_congest_ack_share") == 0;
		found_upload_interface = found_upload_interface || strcmp(name, "ul_if") == 0;
	}
	assert(found_adjust_download);
	assert(found_download_interface);
	assert(found_ping_arguments);
	assert(found_ul_congest_ack_share);
	assert(found_upload_interface);
	assert(config_option_name(config_option_count()) == NULL);
}

static void test_interface_resolution(void)
{
	struct config config = { .interface = "eth0" };
	char error[128] = "";

	assert(resolve_interfaces(&config, error, sizeof(error)) == 0);
	assert(strcmp(config.interface, "eth0") == 0);
	assert(strcmp(config.ingress_interface, "ifb4eth0") == 0);
	assert(!config.interface_overridden);

	config = (struct config){ .interface = "eth0", .ul_if = "wan", .dl_if = "download" };
	assert(resolve_interfaces(&config, error, sizeof(error)) == 0);
	assert(strcmp(config.interface, "wan") == 0);
	assert(strcmp(config.ingress_interface, "download") == 0);
	assert(config.interface_overridden);

	config = (struct config){ .ul_if = "wan", .dl_if = "download" };
	assert(resolve_interfaces(&config, error, sizeof(error)) == 0);
	assert(strcmp(config.interface, "wan") == 0);
	assert(strcmp(config.ingress_interface, "download") == 0);
	assert(!config.interface_overridden);

	config = (struct config){ .interface = "eth0", .ul_if = "wan" };
	assert(resolve_interfaces(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "configured together") != NULL);

	config = (struct config){ .enabled = true };
	assert(resolve_interfaces(&config, error, sizeof(error)) == 0);
	assert(config_validate(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "either option 'interface' or both options") != NULL);
}

static struct config valid_config(void)
{
	return (struct config){ .enabled = true,
				.interface = "eth0",
				.pinger_method = "fping",
				.no_pingers = 1U,
				.reflector_count = 1U,
				.reflectors = { "::1" },
				.reflector_ping_interval_us = 300000U,
				.monitor_achieved_rates_interval_us = 200000U,
				.monitor_cpu_usage_interval_us = 2000000U,
				.bufferbloat_detection_window = 6U,
				.bufferbloat_detection_threshold = 3U,
				.reflector_health_check_interval_us = 1000000U,
				.reflector_response_deadline_us = 1000000U,
				.reflector_misbehaving_detection_window = 60U,
				.reflector_misbehaving_detection_threshold = 3U,
				.stall_detection_threshold = 5U,
				.global_ping_response_timeout_us = 10000000U,
				.interface_up_check_interval_us = 10000000U,
				.download.minimum_rate_bps = 5000000U,
				.download.base_rate_bps = 20000000U,
				.download.maximum_rate_bps = 80000000U,
				.upload.minimum_rate_bps = 5000000U,
				.upload.base_rate_bps = 20000000U,
				.upload.maximum_rate_bps = 35000000U,
				.connection_active_threshold_bps = 2000000U };
}

static void test_supported_pinger_methods(void)
{
	struct config config = valid_config();
	char error[256] = "";

	config.irtt_session_duration_us = 600000000U;
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	(void)snprintf(config.pinger_method, sizeof(config.pinger_method), "irtt");
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	config.irtt_session_duration_us = 0U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "must be positive") != NULL);
	config = valid_config();
	(void)snprintf(config.pinger_method, sizeof(config.pinger_method), "ping");
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strcmp(
		       error,
		       "option 'pinger_method' must be 'fping', 'fping-ts', 'irtt', 'native' or 'native-ts'"
	       ) == 0);

	/* ICMP timestamps are IPv4 only, so fping-ts rejects IPv6 reflectors. */
	config = valid_config();
	(void)snprintf(config.pinger_method, sizeof(config.pinger_method), "fping-ts");
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strcmp(
		       error,
		       "pinger_method 'fping-ts' uses IPv4-only ICMP timestamps; reflector '::1' is IPv6"
	       ) == 0);
	(void)snprintf(config.reflectors[0], sizeof(config.reflectors[0]), "1.1.1.1");
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	(void)snprintf(config.pinger_method, sizeof(config.pinger_method), "fping");
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);

	/* Only fping's shared delay needs TCP-delay attribution. */
	config.tcp_delay_attribution = true;
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	(void)snprintf(config.pinger_method, sizeof(config.pinger_method), "fping-ts");
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strcmp(error,
		      "option 'tcp_delay_attribution' needs pinger_method 'fping' or 'native'") ==
	       0);
	config = valid_config();
	(void)strcpy(config.pinger_method, "native");
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	config.tcp_delay_attribution = true;
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	(void)strcpy(config.ping_extra_args, "-q");
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "ping_extra_args") != NULL);
	config.ping_extra_args[0] = '\0';
	(void)strcpy(config.reflectors[0], "example.com");
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "numeric IP address") != NULL);
	(void)strcpy(config.reflectors[0], "::1");
	config.tcp_delay_attribution = false;
	(void)strcpy(config.pinger_method, "native-ts");
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "IPv4-only") != NULL);
	(void)strcpy(config.reflectors[0], "192.0.2.1");
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
}

static void test_tcp_ts_request(void)
{
	struct uci_section section = { 0 };
	struct uci_option option = {
		.e = { .type = UCI_TYPE_OPTION, .name = "tcp_ts_request" },
		.type = UCI_TYPE_STRING,
		.v.string = "1",
	};
	struct config config = valid_config();
	char error[256] = "";
	const struct loader loader = {
		.section = &section,
		.config = &config,
		.error = error,
		.error_size = sizeof(error),
	};

	(void)snprintf(config.reflectors[0], sizeof(config.reflectors[0]), "1.1.1.1");
	lookup_option = &option;
	assert(load_options(&loader) == 0);
	lookup_option = NULL;
	assert(config.tcp_ts_request);
	/* Injected timestamps only serve the attribution's queue estimate. */
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strcmp(error, "option 'tcp_ts_request' needs tcp_delay_attribution") == 0);
	config.tcp_delay_attribution = true;
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
}

static void test_ul_congest_ack_share(void)
{
	struct uci_section section = { 0 };
	struct uci_option option = {
		.e = { .type = UCI_TYPE_OPTION, .name = "ul_congest_ack_share" },
		.type = UCI_TYPE_STRING,
		.v.string = "0.45",
	};
	struct config config = valid_config();
	char error[256] = "";
	const struct loader loader = {
		.section = &section,
		.config = &config,
		.error = error,
		.error_size = sizeof(error),
	};

	(void)snprintf(config.reflectors[0], sizeof(config.reflectors[0]), "1.1.1.1");
	lookup_option = &option;
	assert(load_options(&loader) == 0);
	lookup_option = NULL;
	assert(config.ul_congest_ack_share_ratio_e6 == 450000U);
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	config.ul_congest_ack_share_ratio_e6 = 1000000U;
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	config.ul_congest_ack_share_ratio_e6 = 1000001U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strcmp(error, "option 'ul_congest_ack_share' must be between 0 and 1") == 0);

	/* A ratio is parsed exactly from its six decimal places. */
	option.e.name = "alpha_baseline_increase";
	option.v.string = "0.001";
	lookup_option = &option;
	assert(load_options(&loader) == 0);
	assert(config.alpha_baseline_increase_ratio_e6 == 1000U);
	option.v.string = "0.0000001";
	assert(load_options(&loader) != 0);
	assert(strcmp(
		       error,
		       "option 'alpha_baseline_increase' has more precision than cake-adapt stores"
	       ) == 0);
	lookup_option = NULL;
}

static void test_reflector_list_validation(void)
{
	struct config config = valid_config();
	char error[256] = "";

	config.no_pingers = 2U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	config.reflector_count = 2U;
	(void)snprintf(config.reflectors[1], sizeof(config.reflectors[1]), "1.1.1.1");
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	assert(validate_reflectors(&config, error, sizeof(error)) == 0);
	/* Each active reflector needs at least a millisecond of the ping interval. */
	config.reflector_ping_interval_us = 1999U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "at least 1 ms per active reflector") != NULL);
	config.reflector_ping_interval_us = 2000U;
	assert(validate_latency_config(&config, error, sizeof(error)) == 0);
	(void)snprintf(config.reflectors[1], sizeof(config.reflectors[1]), "::1");
	assert(validate_reflectors(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "duplicate") != NULL);
	(void)snprintf(config.reflectors[1], sizeof(config.reflectors[1]), "not an endpoint");
	assert(validate_reflectors(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "invalid reflector") != NULL);
}

static void test_new_timer_and_limit_validation(void)
{
	struct config config = valid_config();
	char error[256] = "";

	/* Trackers and reflector health rely on these bounds. */
	config.alpha_delta_ewma_ratio_e6 = 1000001U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "alpha options") != NULL);
	config = valid_config();
	config.reflector_misbehaving_detection_window = 2U;
	config.reflector_misbehaving_detection_threshold = 3U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	assert(strstr(error, "offence threshold") != NULL);
	config.reflector_misbehaving_detection_threshold = 0U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	config = valid_config();

	config.stall_detection_threshold = UINT64_MAX;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	config = valid_config();
	config.global_ping_response_timeout_us = 0U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	config = valid_config();
	config.interface_up_check_interval_us = 0U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	config = valid_config();
	config.output_cpu_stats = true;
	config.monitor_cpu_usage_interval_us = 500U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	config = valid_config();
	config.enable_sleep_function = true;
	config.connection_active_threshold_bps = 5000001U;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
}
static void test_option_copy_boundaries(void)
{
	char value[4] = "old";
	char error[128];
	const struct loader loader = { .error = error, .error_size = sizeof(error) };

	assert(copy_option(&loader, "test", value, sizeof(value), "abc") == 0);
	assert(strcmp(value, "abc") == 0);
	assert(copy_option(&loader, "test", value, sizeof(value), "abcd") == -1);
	assert(strcmp(value, "abc") == 0);
	assert(strstr(error, "too long") != NULL);
	assert(copy_option(&loader, "test", value, sizeof(value), "") == 0);
	assert(value[0] == '\0');
}

#if MQTT_SUPPORTED
/* The MQTT rules of the cake-adapt-mqtt and cake-adapt-mqtt-ssl packages. */
static void test_mqtt_rules(struct mqtt_settings *mqtt)
{
	char error[256];

	mqtt->enabled = true;
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	assert(strstr(error, "mqtt_host") != NULL);
	(void)strcpy(mqtt->host, "broker.example");
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
	mqtt->port = 65536U;
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	mqtt->port = 0U;
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	mqtt->port = 65535U;
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
	(void)strcpy(mqtt->host, "mqtt://broker");
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	(void)strcpy(mqtt->host, "2001:db8::1");
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
	(void)strcpy(mqtt->topic, "logs/+");
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	(void)strcpy(mqtt->topic, "logs/#");
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	mqtt->topic[0] = '\0';
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	(void)strcpy(mqtt->topic, "logs/router");
	(void)strcpy(mqtt->password, "test-password");
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	(void)strcpy(mqtt->username, "test-user");
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
	mqtt->tls = true;
#if MQTT_TLS_SUPPORTED
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
	(void)strcpy(mqtt->ca_file, "relative-ca.pem");
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	assert(strstr(error, "absolute path") != NULL);
	mqtt->ca_file[0] = '\0';
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	(void)strcpy(mqtt->ca_file, "/tmp/fixture-ca.pem");
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
#else
	/* Never plain traffic in place of the TLS asked for. */
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	assert(strcmp(error, "option 'mqtt_tls' requires the cake-adapt-mqtt-ssl package") == 0);
#endif
	mqtt->enabled = false;
	mqtt->ca_file[0] = '\0';
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
	mqtt->enabled = true;
	mqtt->tls = false;
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
}
#endif

static void test_mqtt_validation(void)
{
	struct config config;
	struct mqtt_settings *mqtt = &config.mqtt;
	char error[256];

	defaults_apply(&config);
	assert(!mqtt->enabled);
	assert(!mqtt->tls);
	assert(strcmp(mqtt->ca_file, "/etc/ssl/certs/ca-certificates.crt") == 0);
	assert(mqtt->port == 1883U);
	assert(strcmp(mqtt->topic, "cake-adapt/log") == 0);
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
#if MQTT_SUPPORTED
	test_mqtt_rules(mqtt);
#else
	/* The cake-adapt package keeps the settings and checks no broker setting: it sends nothing. */
	(void)strcpy(mqtt->host, "mqtt://broker");
	mqtt->enabled = true;
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
	config.enabled = false;
	assert(config_validate(&config, error, sizeof(error)) == 0);
	/* TLS asked for is still refused, never replaced by plain traffic. */
	mqtt->tls = true;
	assert(validate_mqtt(mqtt, error, sizeof(error)) != 0);
	assert(strcmp(error, "option 'mqtt_tls' requires the cake-adapt-mqtt-ssl package") == 0);
	mqtt->enabled = false;
	assert(validate_mqtt(mqtt, error, sizeof(error)) == 0);
#endif
}

/* A prefix or extra arguments in the LuCI-writable file are refused; elsewhere, or without them, not. */
static void test_shared_file(void)
{
	struct config config;
	struct stat status;
	char shared[] = "/tmp/cake-adapt-shared-XXXXXX";
	char other[] = "/tmp/cake-adapt-other-XXXXXX";
	char error[256] = "";
	int shared_fd = mkstemp(shared);
	int other_fd = mkstemp(other);

	assert(shared_fd >= 0 && other_fd >= 0);
	defaults_apply(&config);
	assert(stat(shared, &status) == 0);
	assert(config_check_shared_file(&config, &status, shared, error, sizeof(error)) == 0);
	(void)strcpy(config.ping_prefix_string, "/tmp/planted");
	assert(config_check_shared_file(&config, &status, shared, error, sizeof(error)) == -1);
	assert(strstr(error, "option 'ping_prefix_string' is refused in") != NULL);
	assert(strstr(error, "move it to /etc/cake-adapt/exec") != NULL);
	assert(config_check_shared_file(&config, &status, other, error, sizeof(error)) == 0);
	assert(config_check_shared_file(&config, &status, "/nonexistent", error, sizeof(error)) ==
	       0);
	config.ping_prefix_string[0] = '\0';
	(void)strcpy(config.ping_extra_args, "-o /etc/shadow");
	assert(config_check_shared_file(&config, &status, shared, error, sizeof(error)) == -1);
	assert(strstr(error, "option 'ping_extra_args' is refused in") != NULL);
	config.ping_extra_args[0] = '\0';
	(void)strcpy(config.log_file_path_override, "/etc/hotplug.d/iface");
	assert(config_check_shared_file(&config, &status, shared, error, sizeof(error)) == -1);
	assert(strstr(error, "option 'log_file_path_override' is refused in") != NULL);
	(void)close(shared_fd);
	(void)close(other_fd);
	(void)unlink(shared);
	(void)unlink(other);
}

/* A log directory is made absolute; the result must fit the option's 512 bytes. */
static void test_log_directory(void)
{
	struct config config;
	char error[256] = "";

	defaults_apply(&config);
	assert(absolute_log_directory(&config, error, sizeof(error)) == 0);
	assert(config.log_file_path_override[0] == '\0');
	/* 511 bytes, the most the option holds, already absolute. */
	config.log_file_path_override[0] = '/';
	memset(config.log_file_path_override + 1, 'a', 510U);
	config.log_file_path_override[511] = '\0';
	assert(absolute_log_directory(&config, error, sizeof(error)) == 0);
	assert(strlen(config.log_file_path_override) == 511U);
	/* Relative, the working directory in front cannot fit. */
	memset(config.log_file_path_override, 'a', 511U);
	assert(absolute_log_directory(&config, error, sizeof(error)) == -1);
	assert(strstr(error, "option 'log_file_path_override': File name too long") != NULL);
}

/* The value text for option name, as -D prints it. */
static const char *option_text(const struct config *config, const char *name)
{
	static char text[CONFIG_STRING_SIZE];

	for (size_t index = 0U; index < config_option_count(); index++)
		if (strcmp(config_option_name(index), name) == 0) {
			assert(config_option_value(config, index, text, sizeof(text)) == 0);
			return text;
		}
	assert(0);
	return NULL;
}

/* Values in the units UCI writes: shortest decimals, exact, ratios as ratios. */
static void test_option_values_in_uci_units(void)
{
	struct config config = { 0 };
	char text[CONFIG_STRING_SIZE];

	defaults_apply(&config);
	assert(strcmp(option_text(&config, "enabled"), "0") == 0);
	assert(strcmp(option_text(&config, "debug"), "1") == 0);
	assert(strcmp(option_text(&config, "pinger_method"), "native") == 0);
	assert(strcmp(option_text(&config, "mqtt_host"), "") == 0);
	assert(strcmp(option_text(&config, "reflector_ping_interval_s"), "0.3") == 0);
	assert(strcmp(option_text(&config, "min_dl_shaper_rate_kbps"), "5000") == 0);
	assert(strcmp(option_text(&config, "log_file_max_size_KB"), "2000") == 0);
	assert(strcmp(option_text(&config, "reflector_replacement_interval_mins"), "60") == 0);
	assert(strcmp(option_text(&config, "alpha_baseline_increase"), "0.001") == 0);
	assert(strcmp(option_text(&config, "shaper_rate_max_adjust_up_load_high"), "1.04") == 0);
	/* Six places at most, rounded half up, carrying into the whole part. */
	config.reflector_ping_interval_us = 1234567U;
	assert(strcmp(option_text(&config, "reflector_ping_interval_s"), "1.234567") == 0);
	config.log_file_max_size_bytes = 1023U;
	assert(strcmp(option_text(&config, "log_file_max_size_KB"), "0.999023") == 0);
	config.log_file_max_time_us = 2U * MINUTE - 1U;
	assert(strcmp(option_text(&config, "log_file_max_time_mins"), "2") == 0);
	assert(config_option_value(&config, config_option_count(), text, sizeof(text)) == -1);
}

int main(void)
{
	struct config_direction rates;
	uint64_t value = 0U;
	char error[256] = "";

	test_option_values_in_uci_units();
	assert(parse_decimal("18446744073709551615", 1U, &value, error, sizeof(error)) == 0);
	assert(value == UINT64_MAX);
	assert(parse_decimal("18446744073709551616", 1U, &value, error, sizeof(error)) != 0);
	assert(strstr(error, "too large") != NULL);
	/* Representable, but not once scaled. */
	assert(parse_decimal("18446744073709552", 1000U, &value, error, sizeof(error)) != 0);
	assert(strstr(error, "too large") != NULL);
	assert(parse_decimal("1.025", 1000U, &value, error, sizeof(error)) == 0);
	assert(value == 1025U);
	assert(parse_decimal("1.0251", 1000U, &value, error, sizeof(error)) != 0);
	assert(strstr(error, "more precision") != NULL);
	/* Minutes in microseconds: seven decimal places, the last worth 6 us. */
	assert(parse_decimal("0.5", MINUTE, &value, error, sizeof(error)) == 0);
	assert(value == 30000000U);
	assert(parse_decimal("1.0000001", MINUTE, &value, error, sizeof(error)) == 0);
	assert(value == 60000006U);
	assert(parse_decimal("1.00000001", MINUTE, &value, error, sizeof(error)) != 0);
	assert(strstr(error, "more precision") != NULL);
	assert(parse_decimal("307445734561826", MINUTE, &value, error, sizeof(error)) != 0);
	assert(strstr(error, "too large") != NULL);
	/* KB in bytes: 1,024 each. */
	assert(parse_decimal("2000", KILOBYTE, &value, error, sizeof(error)) == 0);
	assert(value == 2048000U);
	assert(parse_decimal("18014398509481984", KILOBYTE, &value, error, sizeof(error)) != 0);
	assert(strstr(error, "too large") != NULL);
	assert(parse_decimal("+1", 1U, &value, error, sizeof(error)) != 0);
	assert(strstr(error, "non-negative decimal") != NULL);

	test_shared_file();
	test_log_directory();
	rates = (struct config_direction){
		.adjust = true,
		.minimum_rate_bps = 5000000U,
		.base_rate_bps = 20000000U,
		.maximum_rate_bps = 80000000U,
	};
	assert(validate_rate_range(&rates, "download", error, sizeof(error)) == 0);
	/* 5000.125 kbit/s is 625,015.625 bytes/s; 5000.008 kbit/s is 625,001. */
	rates.minimum_rate_bps = 5000125U;
	assert(validate_rate_range(&rates, "download", error, sizeof(error)) != 0);
	assert(strstr(error, "whole bytes/s") != NULL);
	rates.minimum_rate_bps = 5000008U;
	assert(validate_rate_range(&rates, "download", error, sizeof(error)) == 0);

	test_supported_pinger_methods();
	test_mqtt_validation();
	test_ul_congest_ack_share();
	test_tcp_ts_request();
	test_option_copy_boundaries();
	test_scalar_option_types();
	test_supported_option_names();
	test_interface_resolution();
	test_reflector_list_validation();
	test_new_timer_and_limit_validation();
	(void)puts("configuration validation tests passed");
	return 0;
}
