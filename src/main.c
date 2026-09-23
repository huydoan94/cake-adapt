#define _GNU_SOURCE

#include "config.h"
#include "log.h"
#include "monitor.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ERROR_SIZE 256U
#define LOG_FILE_NAME "cake-autorate.log"
#define DEFAULT_LOG_DIRECTORY "/var/log"

static void print_usage(const char *program_name)
{
    (void)fprintf(
        stderr,
        "Usage: %s [-f] [-V] [-C UCI_CONFIG_DIRECTORY]\n"
        "       %s -L\n",
        program_name,
        program_name
    );
}

static void randomize_reflector_list(struct config *config)
{
    uint32_t random_values[CONFIG_MAX_REFLECTORS - 1U];
    size_t count = (size_t)config->reflector_count;
    size_t index;

    log_message(LOG_LEVEL_DEBUG, "Randomizing reflectors.");
    if (
        !config->randomize_reflectors ||
        count < 2U
    ) {
        return;
    }
    /* At most 63 words (252 bytes), within getentropy's 256-byte limit. */
    if (getentropy(random_values, (count - 1U) * sizeof(*random_values)) != 0) {
        log_message(
            LOG_LEVEL_WARNING,
            "could not randomize reflectors: %s",
            strerror(errno)
        );
        return;
    }

    for (index = count - 1U; index > 0U; index--) {
        size_t selected = (size_t)(
            random_values[count - 1U - index] % (uint32_t)(index + 1U)
        );
        char temporary[CONFIG_REFLECTOR_SIZE];

        memcpy(temporary, config->reflectors[index], sizeof(temporary));
        memcpy(
            config->reflectors[index],
            config->reflectors[selected],
            sizeof(config->reflectors[index])
        );
        memcpy(
            config->reflectors[selected],
            temporary,
            sizeof(config->reflectors[selected])
        );
    }
}

int main(int argc, char **argv)
{
    struct config config;
    char config_error[ERROR_SIZE] = "";
    const char *config_directory = NULL;
    const char *active_config = "/etc/config/cake-adapt";
    char log_path[CONFIG_STRING_SIZE + sizeof(LOG_FILE_NAME)] =
        DEFAULT_LOG_DIRECTORY "/" LOG_FILE_NAME;
    bool foreground = false;
    bool list_options = false;
    bool validate_only = false;
    size_t index;
    int option;
    int path_length;
    int result;

    while ((option = getopt(argc, argv, "C:LfVh")) != -1) {
        switch (option) {
        case 'C':
            config_directory = optarg;
            break;
        case 'L':
            list_options = true;
            break;
        case 'f':
            foreground = true;
            break;
        case 'V':
            validate_only = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 2;
        }
    }

    if (optind != argc) {
        print_usage(argv[0]);
        return 2;
    }
    if (
        list_options &&
        (config_directory != NULL || foreground || validate_only)
    ) {
        print_usage(argv[0]);
        return 2;
    }
    if (list_options) {
        for (index = 0U; index < config_option_count(); index++) {
            if (puts(config_option_name(index)) == EOF) {
                return 1;
            }
        }
        return 0;
    }
    if (config_directory != NULL) {
        active_config = config_directory;
    }

    log_init("cake-adapt", foreground);

    result = config_load(
        &config,
        config_directory,
        config_error,
        sizeof(config_error)
    );
    if (result != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "configuration error: %s",
            config_error
        );
        log_close();
        return 1;
    }
    if (validate_only) {
        log_close();
        return 0;
    }

    log_set_level(config.debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
    log_set_debug_syslog(config.log_debug_messages_to_syslog);

    if (!config.enabled) {
        log_system_message("cake-adapt is disabled by configuration; exiting");
        log_close();
        return 0;
    }
    if (config.interface_overridden) {
        log_message(
            LOG_LEVEL_WARNING,
            "option 'interface' was overridden by ul_if='%s' and dl_if='%s'",
            config.ul_if,
            config.dl_if
        );
    }

    if (config.log_file_path_override[0] != '\0') {
        path_length = snprintf(
            log_path,
            sizeof(log_path),
            "%s/%s",
            config.log_file_path_override,
            LOG_FILE_NAME
        );
        if (
            path_length < 0 ||
            (size_t)path_length >= sizeof(log_path)
        ) {
            log_message(LOG_LEVEL_ERROR, "log file path is too long");
            log_close();
            return 1;
        }
    }

    if (
        config.log_to_file &&
        log_set_file(
            log_path,
            config.log_file_max_time_minutes,
            config.log_file_max_size_kilobytes,
            config.log_file_buffer_timeout_microseconds,
            config.log_file_export_compress
        ) != 0
    ) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not open log file '%s': %s",
            log_path,
            strerror(errno)
        );
        log_close();
        return 1;
    }

    if (config.cake_autorate_config[0] != '\0') {
        log_message(
            LOG_LEVEL_NOTICE,
            "loaded cake-autorate configuration overrides from '%s'",
            config.cake_autorate_config
        );
    }

    log_print_headers(
        config.output_processing_stats,
        config.output_load_stats,
        config.output_reflector_stats,
        config.output_summary_stats
    );
    log_message(
        LOG_LEVEL_DEBUG,
        "Local list of reflectors contains %" PRIu64 " entries.",
        config.reflector_count
    );
    randomize_reflector_list(&config);

    log_message(
        LOG_LEVEL_INFO,
        "configuration loaded: upload_interface=%s download_interface=%s"
        " reflectors=%" PRIu64 " active_pingers=%" PRIu64
        " reflector_ping_interval=%" PRIu64 " us"
        " traffic_monitor_interval=%" PRIu64 " us"
        " debug=%u log_file=%s",
        config.interface,
        config.ingress_interface,
        config.reflector_count,
        config.no_pingers,
        config.reflector_ping_interval_microseconds,
        config.monitor_achieved_rates_interval_microseconds,
        config.debug ? 1U : 0U,
        config.log_to_file ? log_path : "disabled"
    );
    if (config.adjust_download) {
        log_message(
            LOG_LEVEL_INFO,
            "download adjustment configured: minimum=%" PRIu64
            " bit/s base=%" PRIu64 " bit/s maximum=%" PRIu64 " bit/s",
            config.minimum_download_rate_bits_per_second,
            config.base_download_rate_bits_per_second,
            config.maximum_download_rate_bits_per_second
        );
    }
    if (config.adjust_upload) {
        log_message(
            LOG_LEVEL_INFO,
            "upload adjustment configured: minimum=%" PRIu64
            " bit/s base=%" PRIu64 " bit/s maximum=%" PRIu64 " bit/s",
            config.minimum_upload_rate_bits_per_second,
            config.base_upload_rate_bits_per_second,
            config.maximum_upload_rate_bits_per_second
        );
    }

    log_system_message(
        "Starting cake-adapt with PID: %ld and config: %s",
        (long)getpid(),
        active_config
    );

    if (
        config.adjust_download ||
        config.adjust_upload
    ) {
        log_message(
            LOG_LEVEL_NOTICE,
            "started with CAKE bandwidth control: download=%s upload=%s",
            config.adjust_download ? "enabled" : "disabled",
            config.adjust_upload ? "enabled" : "disabled"
        );
    } else {
        log_message(LOG_LEVEL_NOTICE, "started in observation-only mode");
    }
    result = monitor_run(&config);

    log_system_message(
        "Stopped cake-adapt with PID: %ld and config: %s",
        (long)getpid(),
        active_config
    );
    log_close();
    return result == 0 ? 0 : 1;
}
