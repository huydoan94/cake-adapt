#include "config/describe.h"
#include "config/defaults.h"
#include "logging/log.h"
#include "constants/options.h"
#include "constants/states.h"

#include <inttypes.h>
#include <stdio.h>

int config_print_options(void)
{
	size_t index;

	for (index = 0U; index < config_option_count(); index++)
		if (puts(config_option_name(index)) == EOF)
			return 1;
	return 0;
}

int config_print_defaults(void)
{
	struct config defaults = { 0 };
	char value[CONFIG_STRING_SIZE];

	defaults_apply(&defaults);
	for (size_t index = 0U; index < config_option_count(); index++) {
		(void)config_option_value(&defaults, index, value, sizeof(value));
		if (printf("%s=%s\n", config_option_name(index), value) < 0)
			return 1;
	}
	return 0;
}

void config_log_import(const struct config *config)
{
	if (config->config_file[0] != '\0')
		log_message(
			LOG_LEVEL_NOTICE,
			"config_file '%s': its settings are imported into UCI by the service at start",
			config->config_file
		);
}

static void log_adjustment(const char *name, const struct config_direction *direction)
{
	if (!direction->adjust)
		return;
	log_message(
		LOG_LEVEL_INFO,
		"%s adjustment configured: minimum=%" PRIu64 " bit/s base=%" PRIu64
		" bit/s maximum=%" PRIu64 " bit/s",
		name,
		direction->minimum_rate_bps,
		direction->base_rate_bps,
		direction->maximum_rate_bps
	);
}

void config_log_loaded(const struct config *config, const char *log_path)
{
	log_message(
		LOG_LEVEL_DEBUG,
		"Local list of reflectors contains %" PRIu64 " entries.",
		config->reflector_count
	);
	log_message(
		LOG_LEVEL_INFO,
		"configuration loaded: upload_interface=%s download_interface=%s"
		" reflectors=%" PRIu64 " active_pingers=%" PRIu64
		" reflector_ping_interval=%" PRIu64 " us"
		" traffic_monitor_interval=%" PRIu64 " us"
		" debug=%u log_file=%s",
		config->interface,
		config->ingress_interface,
		config->reflector_count,
		config->no_pingers,
		config->reflector_ping_interval_us,
		config->monitor_achieved_rates_interval_us,
		config->debug ? 1U : 0U,
		config->log_to_file ? log_path : STATUS_DISABLED
	);
	log_adjustment(DIRECTION_DOWNLOAD, &config->download);
	log_adjustment(DIRECTION_UPLOAD, &config->upload);
}

void config_log_control_mode(const struct config *config)
{
	const struct config_direction *download = &config->download;
	const struct config_direction *upload = &config->upload;

	if (config_adjusts(config))
		log_message(
			LOG_LEVEL_NOTICE,
			"started with CAKE bandwidth control: download=%s upload=%s",
			download->adjust ? STATUS_ENABLED : STATUS_DISABLED,
			upload->adjust ? STATUS_ENABLED : STATUS_DISABLED
		);
	else
		log_message(LOG_LEVEL_NOTICE, "started in observation-only mode");
}
