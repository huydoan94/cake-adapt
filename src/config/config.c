#define _GNU_SOURCE

#include "config/config.h"
#include "common/constants.h"
#include "config/defaults.h"
#include "common/error.h"
#include "common/helpers.h"

#include <libubox/utils.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Current libuci headers contain inline helpers that trigger -Wsign-conversion.
 * Keep strict conversion warnings for cake-adapt while isolating that external
 * header warning.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include <uci.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

enum option_type {
	TYPE_BOOLEAN,
	TYPE_STRING,
	/* A non-negative decimal stored multiplied by scale, such as seconds in microseconds. */
	TYPE_SCALED,
};

struct option_binding {
	const char *name;
	enum option_type type;
	size_t offset;
	size_t size;
	uint64_t scale;
};

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
	BOOLEAN_OPTION(OPTION_RANDOMIZE_REFLECTORS, randomize_reflectors),
	BOOLEAN_OPTION(OPTION_RETAIN_REFLECTOR_STATS, retain_reflector_stats),
	BOOLEAN_OPTION(OPTION_ENABLE_SLEEP_FUNCTION, enable_sleep_function),
	BOOLEAN_OPTION(OPTION_MIN_SHAPER_RATES_ENFORCEMENT, minimum_shaper_rates_enforcement),
	BOOLEAN_OPTION(OPTION_LOG_FILE_EXPORT_COMPRESS, log_file_export_compress),
	BOOLEAN_OPTION(OPTION_TCP_DELAY_ATTRIBUTION, tcp_delay_attribution),
	STRING_OPTION(OPTION_INTERFACE, interface),
	STRING_OPTION(OPTION_UPLOAD_INTERFACE, ul_if),
	STRING_OPTION(OPTION_DOWNLOAD_INTERFACE, dl_if),
	STRING_OPTION(OPTION_CONFIG_FILE, config_file),
	STRING_OPTION(OPTION_LOG_FILE_PATH_OVERRIDE, log_file_path_override),
	STRING_OPTION(OPTION_PINGER_METHOD, pinger_method),
	STRING_OPTION(OPTION_PING_EXTRA_ARGS, ping_extra_args),
	STRING_OPTION(OPTION_PING_PREFIX_STRING, ping_prefix_string),
	SCALED_OPTION(OPTION_LOG_FILE_MAX_TIME, log_file_max_time_us, MINUTE),
	SCALED_OPTION(OPTION_LOG_FILE_MAX_SIZE, log_file_max_size_bytes, KILOBYTE),
	SCALED_OPTION(OPTION_NO_PINGERS, no_pingers, 1U),
	SCALED_OPTION(OPTION_REFLECTOR_PING_INTERVAL, reflector_ping_interval_us, SECOND),
	SCALED_OPTION(OPTION_MIN_DOWNLOAD_RATE, download.minimum_rate_bits_per_second, KILOBIT),
	SCALED_OPTION(OPTION_BASE_DOWNLOAD_RATE, download.base_rate_bits_per_second, KILOBIT),
	SCALED_OPTION(OPTION_MAX_DOWNLOAD_RATE, download.maximum_rate_bits_per_second, KILOBIT),
	SCALED_OPTION(OPTION_MIN_UPLOAD_RATE, upload.minimum_rate_bits_per_second, KILOBIT),
	SCALED_OPTION(OPTION_BASE_UPLOAD_RATE, upload.base_rate_bits_per_second, KILOBIT),
	SCALED_OPTION(OPTION_MAX_UPLOAD_RATE, upload.maximum_rate_bits_per_second, KILOBIT),
	SCALED_OPTION(
		OPTION_CONNECTION_ACTIVE_THRESHOLD,
		connection_active_threshold_bits_per_second,
		KILOBIT
	),
	SCALED_OPTION(
		OPTION_CONNECTION_STALL_THRESHOLD,
		connection_stall_threshold_bits_per_second,
		KILOBIT
	),
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

size_t config_option_count(void)
{
	return ARRAY_SIZE(options);
}

const char *config_option_name(size_t index)
{
	return index < ARRAY_SIZE(options) ? options[index].name : NULL;
}

/* The section being loaded, the configuration it fills and where a failure is described. */
struct loader {
	struct uci_context *context;
	struct uci_section *section;
	struct config *config;
	char *error;
	size_t error_size;
};

static int copy_option(
	const struct loader *loader,
	const char *name,
	char *destination,
	size_t destination_size,
	const char *value
)
{
	size_t length = strlen(value);

	if (length >= destination_size)
		return error_set(loader->error, loader->error_size, "option '%s' is too long", name);

	memcpy(destination, value, length + 1U);
	return 0;
}

static int resolve_interfaces(struct config *config, char *error, size_t error_size)
{
	bool upload_configured = config->ul_if[0] != '\0';
	bool download_configured = config->dl_if[0] != '\0';

	config->interface_overridden = false;
	if (upload_configured != download_configured) {
		return error_set(
			error,
			error_size,
			"options 'ul_if' and 'dl_if' must be configured together"
		);
	}
	/* The mismatch check above guarantees dl_if is configured here too. */
	if (upload_configured) {
		config->interface_overridden = config->interface[0] != '\0';
		memcpy(config->interface, config->ul_if, sizeof(config->interface));
		memcpy(config->ingress_interface, config->dl_if, sizeof(config->ingress_interface));
		return 0;
	}

	config->ingress_interface[0] = '\0';
	if (config->interface[0] == '\0')
		return 0;

	/* SQM names its ingress IFB "ifb4<interface>", truncated to IFNAMSIZ. */
	(void)snprintf(
		config->ingress_interface,
		sizeof(config->ingress_interface),
		IFB_PREFIX "%.*s",
		(int)(sizeof(config->ingress_interface) - sizeof(IFB_PREFIX)),
		config->interface
	);
	return 0;
}

static int parse_boolean(const char *value, bool *result)
{
	if (strcmp(value, BOOLEAN_TRUE_NUMERIC) == 0 || strcasecmp(value, BOOLEAN_TRUE) == 0 ||
	    strcasecmp(value, BOOLEAN_YES) == 0 || strcasecmp(value, BOOLEAN_ON) == 0) {
		*result = true;
		return 0;
	}

	if (strcmp(value, BOOLEAN_FALSE_NUMERIC) == 0 || strcasecmp(value, BOOLEAN_FALSE) == 0 ||
	    strcasecmp(value, BOOLEAN_NO) == 0 || strcasecmp(value, BOOLEAN_OFF) == 0) {
		*result = false;
		return 0;
	}

	return -1;
}

/* A scalar option's value, or NULL when the section does not set it. */
static int lookup_string_option(const struct loader *loader, const char *name, const char **value)
{
	struct uci_option *option = uci_lookup_option(loader->context, loader->section, name);

	*value = NULL;
	if (option == NULL)
		return 0;
	if (option->type != UCI_TYPE_STRING) {
		return error_set(
			loader->error,
			loader->error_size,
			"option '%s' must be a scalar UCI option, not a list",
			name
		);
	}
	*value = option->v.string;
	return 0;
}

static int parse_scaled_decimal(
	const struct loader *loader,
	const struct option_binding *option,
	const char *value,
	uint64_t *result
)
{
	const char *character = value + strspn(value, DECIMAL_DIGITS);
	uint64_t scaled_value;
	uint64_t fractional_place = option->scale;

	if (value[0] == '\0')
		return error_set(
			loader->error,
			loader->error_size,
			"option '%s' is empty",
			option->name
		);
	if (character == value)
		goto invalid;
	if (!parse_unsigned(value, character, &scaled_value) ||
	    scaled_value > UINT64_MAX / option->scale)
		goto too_large;
	scaled_value *= option->scale;

	if (*character != '\0') {
		/* A point followed by one or more digits. */
		if (*character != '.' || character[1] == '\0' ||
		    character[1 + strspn(character + 1, DECIMAL_DIGITS)] != '\0') {
			goto invalid;
		}
		for (character++; *character != '\0'; character++) {
			uint64_t digit = (uint64_t)(*character - '0');

			/* The next place would not be a whole number of stored units. */
			if (fractional_place % 10U != 0U) {
				if (digit != 0U) {
					return error_set(
						loader->error,
						loader->error_size,
						"option '%s' has more precision than cake-adapt stores",
						option->name
					);
				}
				continue;
			}
			fractional_place /= 10U;
			if (scaled_value > UINT64_MAX - digit * fractional_place)
				goto too_large;
			scaled_value += digit * fractional_place;
		}
	}
	*result = scaled_value;
	return 0;

invalid:
	return error_set(
		loader->error,
		loader->error_size,
		"option '%s' is not a non-negative decimal",
		option->name
	);
too_large:
	return error_set(
		loader->error,
		loader->error_size,
		"option '%s' is too large",
		option->name
	);
}

static int
load_option(const struct loader *loader, const struct option_binding *option, const char *value)
{
	char *destination = (char *)loader->config + option->offset;

	switch (option->type) {
	case TYPE_BOOLEAN:
		if (parse_boolean(value, (bool *)destination) != 0)
			return error_set(
				loader->error,
				loader->error_size,
				"option '%s' is not a boolean",
				option->name
			);
		return 0;
	case TYPE_STRING:
		return copy_option(loader, option->name, destination, option->size, value);
	case TYPE_SCALED:
		return parse_scaled_decimal(loader, option, value, (uint64_t *)destination);
	}
	return 0;
}

static int load_options(const struct loader *loader)
{
	size_t index;

	for (index = 0U; index < ARRAY_SIZE(options); index++) {
		const char *value;

		if (lookup_string_option(loader, options[index].name, &value) != 0)
			return -1;
		if (value != NULL && load_option(loader, &options[index], value) != 0)
			return -1;
	}
	return 0;
}

static int copy_reflector(const struct loader *loader, const char *reflector)
{
	struct config *config = loader->config;
	uint64_t index = config->reflector_count;

	if (index >= CONFIG_MAX_REFLECTORS) {
		return error_set(
			loader->error,
			loader->error_size,
			"option 'reflectors' contains more than %u entries",
			CONFIG_MAX_REFLECTORS
		);
	}
	if (copy_option(
		    loader,
		    OPTION_REFLECTORS,
		    config->reflectors[index],
		    sizeof(config->reflectors[index]),
		    reflector
	    ) != 0) {
		return -1;
	}
	config->reflector_count++;
	return 0;
}

static int load_reflectors(const struct loader *loader)
{
	struct uci_option *option =
		uci_lookup_option(loader->context, loader->section, OPTION_REFLECTORS);
	struct uci_element *element;

	if (option == NULL)
		return 0;
	if (option->type != UCI_TYPE_LIST)
		return error_set(
			loader->error,
			loader->error_size,
			"option 'reflectors' must be a UCI list"
		);

	loader->config->reflector_count = 0U;
	uci_foreach_element(&option->v.list, element) {
		if (copy_reflector(loader, element->name) != 0)
			return -1;
	}
	return 0;
}

static int load_section(const struct loader *loader)
{
	struct config *config = loader->config;
	bool reflectors_configured =
		uci_lookup_option(loader->context, loader->section, OPTION_REFLECTORS) != NULL;
	bool no_pingers_configured =
		uci_lookup_option(loader->context, loader->section, OPTION_NO_PINGERS) != NULL;
	/* Legacy single-target input is needed only while loading configuration. */
	char latency_target[CONFIG_REFLECTOR_SIZE] = { 0 };
	const char *latency_target_value;

	if (lookup_string_option(loader, OPTION_LATENCY_TARGET, &latency_target_value) != 0 ||
	    (latency_target_value != NULL && copy_option(
						     loader,
						     OPTION_LATENCY_TARGET,
						     latency_target,
						     sizeof(latency_target),
						     latency_target_value
					     ) != 0)) {
		return -1;
	}

	if (load_options(loader) != 0 || load_reflectors(loader) != 0 ||
	    resolve_interfaces(config, loader->error, loader->error_size) != 0)
		return -1;

	if (!reflectors_configured && latency_target[0] != '\0') {
		config->reflector_count = 0U;
		if (copy_reflector(loader, latency_target) != 0)
			return -1;
		if (!no_pingers_configured)
			config->no_pingers = 1U;
	}

	return config_validate(config, loader->error, loader->error_size);
}

int config_load(
	struct config *config,
	const char *config_directory,
	const char *section_name,
	char *error,
	size_t error_size
)
{
	struct loader loader = {
		.config = config,
		.error = error,
		.error_size = error_size,
	};
	struct uci_context *context;
	struct uci_package *package = NULL;
	int result = -1;

	if (section_name == NULL || section_name[0] == '\0')
		return error_set(error, error_size, "UCI section name is empty");

	defaults_apply(config);

	context = uci_alloc_context();
	if (context == NULL)
		return error_set(error, error_size, "could not allocate a UCI context");

	if (config_directory != NULL) {
		if (uci_set_confdir(context, config_directory) != UCI_OK) {
			error_set(
				error,
				error_size,
				"could not use UCI configuration directory '%s'",
				config_directory
			);
			goto done;
		}
	}

	if (uci_load(context, UCI_PACKAGE, &package) != UCI_OK) {
		char *uci_error = NULL;

		uci_get_errorstr(context, &uci_error, UCI_PACKAGE);
		error_set(
			error,
			error_size,
			"%s",
			uci_error != NULL ? uci_error : "could not load UCI configuration"
		);
		free(uci_error);
		goto done;
	}

	loader.context = context;
	loader.section = uci_lookup_section(context, package, section_name);
	if (loader.section == NULL || strcmp(loader.section->type, UCI_SECTION_TYPE) != 0) {
		error_set(error, error_size, "missing config cake_adapt '%s' section", section_name);
		goto done;
	}

	result = load_section(&loader);

done:
	/* libuci owns and releases every package loaded into this context. */
	uci_free_context(context);
	return result;
}
