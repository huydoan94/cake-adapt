#define _POSIX_C_SOURCE 200809L

#include "config/config.h"
#include "common/helpers.h"
#include "config/defaults.h"
#include "config/describe.h"
#include "common/error.h"
#include "latency/latency.h"
#include "logging/log.h"
#include "logging/records.h"
#include "logging/mqtt.h"
#include "monitor/monitor.h"
#include "platform/instance.h"
#include "constants/logging.h"
#include "constants/program.h"
#include "constants/text.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *program_name)
{
	(void)fprintf(
		stderr,
		"Usage: %s [-f] [-V] [-C UCI_CONFIG_FILE] [-S UCI_SECTION]\n"
		"       %s -L | -D\n",
		program_name,
		program_name
	);
}

struct options {
	const char *config_path;
	const char *section_name;
	bool foreground;
	/* -L lists the options, -D their built-in defaults. */
	bool list;
	bool defaults;
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
			options->config_path = optarg;
			break;
		case 'L':
			options->list = true;
			break;
		case 'D':
			options->defaults = true;
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
	/* -L and -D each stand alone. */
	if (optind != argc ||
	    ((options->list || options->defaults) &&
	     (options->list == options->defaults || options->config_path != NULL ||
	      options->foreground || options->section_selected || options->validate_only))) {
		print_usage(argv[0]);
		return 2;
	}
	return -1;
}

/* The record types the log file holds; their headers start it and every rotated one. */
static void print_record_headers(const struct config *config)
{
	const struct log_records records = {
		.data = config->output_processing_stats,
		.load = config->output_load_stats,
		.reflector = config->output_reflector_stats,
		.summary = config->output_summary_stats,
		.tcp_queue = config->output_processing_stats && config->tcp_delay_attribution,
		.tcp_inject = config->output_processing_stats && config->tcp_ts_request,
		.memory = config->output_memory_stats,
	};

	log_print_headers(&records);
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
	const char *separator = EMPTY_STRING;
	const char *instance = EMPTY_STRING;
	const struct log_file_settings settings = {
		.maximum_time_us = config->log_file_max_time_us,
		.maximum_size_bytes = config->log_file_max_size_bytes,
		.buffer_timeout_us = config->log_file_buffer_timeout_us,
		.compress_exports = config->log_file_export_compress,
	};
	int length;

	if (strcmp(section_name, DEFAULT_SECTION) != 0) {
		separator = LOG_SECTION_SEPARATOR;
		instance = section_name;
	}
	length = snprintf(
		log_path,
		log_path_size,
		"%s/%s%s%s" LOG_EXTENSION,
		directory,
		DEFAULT_LOG_FILE_BASE,
		separator,
		instance
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

int main(int argc, char **argv)
{
	struct options options = { .section_name = DEFAULT_SECTION };
	char error[ERROR_SIZE] = { 0 };
	/* The lock's messages name the lock path, which may be long. */
	char message[LOG_MESSAGE_SIZE];
	char log_path[LOG_PATH_SIZE];
	char config_path[PATH_MAX];
	const char *requested_config;
	const char *active_config = config_path;
	struct config config;
	enum pinger_method pinger_method;
	int ret;

	ret = parse_options(argc, argv, &options);
	if (ret >= 0)
		return ret;
	if (options.list)
		return config_print_options();
	if (options.defaults)
		return config_print_defaults();
	requested_config = options.config_path ? options.config_path : DEFAULT_CONFIG_PATH;

	log_init(PROGRAM_NAME, options.foreground);
	if (options.validate_only)
		log_problems_to_stderr();

	ret = 1;
	if (absolute_path(requested_config, config_path, sizeof(config_path)) != 0) {
		log_message(
			LOG_LEVEL_ERROR,
			"configuration error: path '%s': %s",
			requested_config,
			strerror(errno)
		);
		goto out;
	}
	if (config_load(&config, active_config, options.section_name, error, sizeof(error)) != 0) {
		log_message(LOG_LEVEL_ERROR, "configuration error: %s", error);
		goto out;
	}
	pinger_method = latency_method(config.pinger_method);
	if (config.enabled && latency_check_backend(pinger_method, error, sizeof(error)) != 0) {
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
	/* Before the log file, which belongs to a running instance of the same name. */
	if (instance_lock(options.section_name, message, sizeof(message)) != 0) {
		log_message(LOG_LEVEL_ERROR, "%s", message);
		goto out;
	}
	if (open_log_file(&config, options.section_name, log_path, sizeof(log_path)) != 0)
		goto out;
	if (mqtt_start(&config.mqtt, error, sizeof(error)) != 0)
		log_message(LOG_LEVEL_WARNING, "MQTT logging degraded: %s", error);
	/* From here on, slow storage or a stuck logd cannot hold up measurement and control. */
	if (log_start_writer(error, sizeof(error)) != 0)
		log_message(
			LOG_LEVEL_WARNING,
			"logging directly, without its own thread: %s",
			error
		);
	config_log_import(&config);
	print_record_headers(&config);
	config_log_loaded(&config, log_path);

	log_system_message(
		"Starting cake-adapt %s with PID: %ld, config: %s, section: %s",
		PROGRAM_VERSION,
		(long)getpid(),
		active_config,
		options.section_name
	);
	config_log_control_mode(&config);
	ret = monitor_run(&config, options.section_name) == 0 ? 0 : 1;
	log_system_message(
		"Stopped cake-adapt with PID: %ld, config: %s, section: %s",
		(long)getpid(),
		active_config,
		options.section_name
	);
out:
	if (!log_close())
		_exit(ret);
	return ret;
}
