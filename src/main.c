#include "config/config.h"
#include "common/constants.h"
#include "config/defaults.h"
#include "common/error.h"
#include "latency/latency.h"
#include "logging/log.h"
#include "monitor/monitor.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *program_name)
{
	(void)fprintf(
		stderr,
		"Usage: %s [-f] [-V] [-C UCI_CONFIG_DIRECTORY] [-S UCI_SECTION]\n"
		"       %s -L\n",
		program_name,
		program_name
	);
}

struct options {
	const char *config_directory;
	const char *section_name;
	bool foreground;
	bool list;
	bool section_selected;
	bool validate_only;
};

/* Returns -1 to continue, or the exit code. */
static int parse_options(int argc, char **argv, struct options *options)
{
	int option;

	while ((option = getopt(argc, argv, CLI_OPTIONS)) != -1) {
		switch (option) {
		case 'C':
			options->config_directory = optarg;
			break;
		case 'L':
			options->list = true;
			break;
		case 'f':
			options->foreground = true;
			break;
		case 'S':
			options->section_name = optarg;
			options->section_selected = true;
			break;
		case 'V':
			options->validate_only = true;
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 2;
		}
	}
	if (optind != argc ||
	    (options->list && (options->config_directory != NULL || options->foreground ||
			       options->section_selected || options->validate_only))) {
		print_usage(argv[0]);
		return 2;
	}
	return -1;
}

static int list_config_options(void)
{
	size_t index;

	for (index = 0U; index < config_option_count(); index++)
		if (puts(config_option_name(index)) == EOF)
			return 1;
	return 0;
}

static int open_log_file(
	const struct config *config,
	const char *section_name,
	char *log_path,
	size_t log_path_size
)
{
	const char *directory = config->log_file_path_override[0] != '\0' ?
					config->log_file_path_override :
					DEFAULT_LOG_DIRECTORY;
	/* cake-adapt.log for the main section, cake-adapt.<section>.log otherwise. */
	bool named = strcmp(section_name, DEFAULT_SECTION) != 0;
	const struct log_file_settings settings = {
		.maximum_time_minutes = config->log_file_max_time_minutes,
		.maximum_size_kilobytes = config->log_file_max_size_kilobytes,
		.buffer_timeout_microseconds = config->log_file_buffer_timeout_microseconds,
		.compress_exports = config->log_file_export_compress,
	};
	int length = snprintf(
		log_path,
		log_path_size,
		"%s/%s%s%s" LOG_EXTENSION,
		directory,
		DEFAULT_LOG_FILE_BASE,
		named ? "." : "",
		named ? section_name : ""
	);
	if (length < 0 || (size_t)length >= log_path_size) {
		log_message(LOG_LEVEL_ERROR, "log file path is too long");
		return -1;
	}
	if (config->log_to_file && log_set_file(log_path, &settings) != 0) {
		log_message(
			LOG_LEVEL_ERROR,
			"could not open log file '%s': %s",
			log_path,
			strerror(errno)
		);
		return -1;
	}
	return 0;
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
		direction->minimum_rate_bits_per_second,
		direction->base_rate_bits_per_second,
		direction->maximum_rate_bits_per_second
	);
}

static void log_configuration(const struct config *config, const char *log_path)
{
	const struct log_records records = {
		.data = config->output_processing_stats,
		.load = config->output_load_stats,
		.reflector = config->output_reflector_stats,
		.summary = config->output_summary_stats,
		.tcp_queue = config->output_processing_stats && config->tcp_delay_attribution,
	};

	if (config->config_file[0] != '\0')
		log_message(
			LOG_LEVEL_NOTICE,
			"loaded configuration overrides from '%s'",
			config->config_file
		);

	log_print_headers(&records);
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
		config->reflector_ping_interval_microseconds,
		config->monitor_achieved_rates_interval_microseconds,
		config->debug ? 1U : 0U,
		config->log_to_file ? log_path : STATUS_DISABLED
	);
	log_adjustment(DIRECTION_DOWNLOAD, &config->download);
	log_adjustment(DIRECTION_UPLOAD, &config->upload);
}

static void log_control_mode(const struct config *config)
{
	if (config->download.adjust || config->upload.adjust)
		log_message(
			LOG_LEVEL_NOTICE,
			"started with CAKE bandwidth control: download=%s upload=%s",
			config->download.adjust ? STATUS_ENABLED : STATUS_DISABLED,
			config->upload.adjust ? STATUS_ENABLED : STATUS_DISABLED
		);
	else
		log_message(LOG_LEVEL_NOTICE, "started in observation-only mode");
}

int main(int argc, char **argv)
{
	struct options options = { .section_name = DEFAULT_SECTION };
	char error[ERROR_SIZE] = { 0 };
	char log_path[LOG_PATH_SIZE];
	const char *active_config;
	struct config config;
	int ret;

	ret = parse_options(argc, argv, &options);
	if (ret >= 0)
		return ret;
	if (options.list)
		return list_config_options();
	active_config = options.config_directory ? options.config_directory : DEFAULT_CONFIG_PATH;

	log_init(PROGRAM_NAME, options.foreground);

	ret = 1;
	if (config_load(
		    &config,
		    options.config_directory,
		    options.section_name,
		    error,
		    sizeof(error)
	    ) != 0) {
		log_message(LOG_LEVEL_ERROR, "configuration error: %s", error);
		goto out;
	}
	if (config.enabled &&
	    latency_check_backend(config.pinger_method, error, sizeof(error)) != 0) {
		log_message(LOG_LEVEL_ERROR, "%s; exiting", error);
		goto out;
	}
	ret = 0;
	if (options.validate_only)
		goto out;

	log_set_level(config.debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
	log_set_debug_syslog(config.log_debug_messages_to_syslog);

	if (!config.enabled) {
		log_system_message("cake-adapt is disabled by configuration; exiting");
		goto out;
	}
	if (config.interface_overridden)
		log_message(
			LOG_LEVEL_WARNING,
			"option 'interface' was overridden by ul_if='%s' and dl_if='%s'",
			config.ul_if,
			config.dl_if
		);

	ret = 1;
	if (open_log_file(&config, options.section_name, log_path, sizeof(log_path)) != 0)
		goto out;
	log_configuration(&config, log_path);

	log_system_message(
		"Starting cake-adapt %s with PID: %ld, config: %s, section: %s",
		PROGRAM_VERSION,
		(long)getpid(),
		active_config,
		options.section_name
	);
	log_control_mode(&config);
	ret = monitor_run(&config) == 0 ? 0 : 1;
	log_system_message(
		"Stopped cake-adapt with PID: %ld, config: %s, section: %s",
		(long)getpid(),
		active_config,
		options.section_name
	);
out:
	log_close();
	return ret;
}
