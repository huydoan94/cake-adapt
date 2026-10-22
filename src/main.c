#define _POSIX_C_SOURCE 200809L

#include "config/config.h"
#include "common/constants.h"
#include "common/helpers.h"
#include "config/defaults.h"
#include "common/error.h"
#include "latency/latency.h"
#include "logging/log.h"
#include "logging/mqtt.h"
#include "monitor/monitor.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

/* "name=value" for each option, as a section that sets none runs. */
static int list_config_defaults(void)
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

/*
 * One daemon per instance name: a second one would control the same CAKE
 * qdiscs. The lock lasts until the process exits, so its descriptor is never
 * closed; it is close-on-exec, so pinger children do not hold it.
 */
static int lock_instance(const char *section_name)
{
	char path[PATH_MAX];
	long holder_pid;
	int length = snprintf(
		path,
		sizeof(path),
		"%s/%s" LOCK_EXTENSION,
		RUNTIME_DIRECTORY,
		section_name
	);

	if (length < 0 || (size_t)length >= sizeof(path)) {
		log_message(LOG_LEVEL_ERROR, "instance '%s': lock path is too long", section_name);
		return -1;
	}
	if (mkdir(RUNTIME_DIRECTORY, 0700) != 0 && errno != EEXIST) {
		log_message(
			LOG_LEVEL_ERROR,
			"could not create %s: %s; exiting",
			RUNTIME_DIRECTORY,
			strerror(errno)
		);
		return -1;
	}
	if (lock_pid_file(path, &holder_pid) >= 0)
		return 0;
	if (errno != EWOULDBLOCK)
		log_message(
			LOG_LEVEL_ERROR,
			"could not lock %s: %s; exiting",
			path,
			strerror(errno)
		);
	else if (holder_pid > 0)
		log_message(
			LOG_LEVEL_ERROR,
			"instance '%s' is already running (pid %ld); exiting",
			section_name,
			holder_pid
		);
	else
		log_message(
			LOG_LEVEL_ERROR,
			"instance '%s' is already running; exiting",
			section_name
		);
	return -1;
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

static void log_configuration(const struct config *config, const char *log_path)
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

	if (config->config_file[0] != '\0')
		log_message(
			LOG_LEVEL_NOTICE,
			"config_file '%s': its settings are imported into UCI by the service at start",
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
		config->reflector_ping_interval_us,
		config->monitor_achieved_rates_interval_us,
		config->debug ? 1U : 0U,
		config->log_to_file ? log_path : STATUS_DISABLED
	);
	log_adjustment(DIRECTION_DOWNLOAD, &config->download);
	log_adjustment(DIRECTION_UPLOAD, &config->upload);
}

static void log_control_mode(const struct config *config)
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

int main(int argc, char **argv)
{
	struct options options = { .section_name = DEFAULT_SECTION };
	char error[ERROR_SIZE] = { 0 };
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
		return list_config_options();
	if (options.defaults)
		return list_config_defaults();
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
	if (lock_instance(options.section_name) != 0)
		goto out;
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
	log_configuration(&config, log_path);

	log_system_message(
		"Starting cake-adapt %s with PID: %ld, config: %s, section: %s",
		PROGRAM_VERSION,
		(long)getpid(),
		active_config,
		options.section_name
	);
	log_control_mode(&config);
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
