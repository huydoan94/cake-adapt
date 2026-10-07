#include "config/config.c"
#include "config/validate.c"
#include "common/utils.h"

#include <assert.h>

/* Use real libuci inline accessors with a controlled single-option lookup. */
static struct uci_option *lookup_option;

int uci_lookup_next(
	struct uci_context *context,
	struct uci_element **element,
	struct uci_list *list,
	const char *name
)
{
	(void)context;
	(void)list;
	if (lookup_option == NULL || strcmp(lookup_option->e.name, name) != 0)
		return UCI_ERR_NOTFOUND;
	*element = &lookup_option->e;
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
				.download.minimum_rate_bits_per_second = 5000000U,
				.download.base_rate_bits_per_second = 20000000U,
				.download.maximum_rate_bits_per_second = 80000000U,
				.upload.minimum_rate_bits_per_second = 5000000U,
				.upload.base_rate_bits_per_second = 20000000U,
				.upload.maximum_rate_bits_per_second = 35000000U,
				.connection_active_threshold_bits_per_second = 2000000U };
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
	assert(strcmp(error, "option 'pinger_method' must be 'fping', 'fping-ts' or 'irtt'") == 0);

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
	assert(strcmp(error, "option 'tcp_delay_attribution' needs pinger_method 'fping'") == 0);
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
	config.log_file_max_size_kilobytes = UINT64_MAX;
	assert(validate_latency_config(&config, error, sizeof(error)) != 0);
	config = valid_config();
	config.enable_sleep_function = true;
	config.connection_active_threshold_bits_per_second = 5000001U;
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

int main(void)
{
	struct config_direction rates;
	uint64_t value = 0U;
	char error[256] = "";

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
	assert(parse_decimal("+1", 1U, &value, error, sizeof(error)) != 0);
	assert(strstr(error, "non-negative decimal") != NULL);

	rates = (struct config_direction){
		.adjust = true,
		.minimum_rate_bits_per_second = 5000000U,
		.base_rate_bits_per_second = 20000000U,
		.maximum_rate_bits_per_second = 80000000U,
	};
	assert(validate_rate_range(&rates, "download", error, sizeof(error)) == 0);
	/* 5000.125 kbit/s is 625,015.625 bytes/s; 5000.008 kbit/s is 625,001. */
	rates.minimum_rate_bits_per_second = 5000125U;
	assert(validate_rate_range(&rates, "download", error, sizeof(error)) != 0);
	assert(strstr(error, "whole bytes/s") != NULL);
	rates.minimum_rate_bits_per_second = 5000008U;
	assert(validate_rate_range(&rates, "download", error, sizeof(error)) == 0);

	test_supported_pinger_methods();
	test_ul_congest_ack_share();
	test_option_copy_boundaries();
	test_scalar_option_types();
	test_supported_option_names();
	test_interface_resolution();
	test_reflector_list_validation();
	test_new_timer_and_limit_validation();
	(void)puts("configuration validation tests passed");
	return 0;
}
