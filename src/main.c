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
#include <sys/random.h>
#include <unistd.h>

#define ERROR_SIZE 256U

static void print_usage(const char *program_name)
{
    (void)fprintf(
        stderr,
        "Usage: %s [-f] [-C UCI_CONFIG_DIRECTORY]\n",
        program_name
    );
}

static bool read_random_values(uint32_t *values, size_t count)
{
    unsigned char *destination = (unsigned char *)(void *)values;
    size_t expected = count * sizeof(*values);
    size_t received = 0U;

    while (received < expected) {
        ssize_t result = getrandom(
            destination + received,
            expected - received,
            0
        );

        if (result > 0) {
            received += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            errno = EIO;
        }
        return false;
    }
    return true;
}

static void randomize_reflector_list(struct sqm_mon_config *config)
{
    uint32_t random_values[CONFIG_MAX_REFLECTORS - 1U];
    size_t count = (size_t)config->reflector_count;
    size_t index;

    log_message(LOG_LEVEL_DEBUG, "Randomizing reflectors.");
    if (!config->randomize_reflectors || count < 2U) {
        return;
    }
    if (!read_random_values(random_values, count - 1U)) {
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
    struct sqm_mon_config config;
    char config_error[ERROR_SIZE] = "";
    const char *config_directory = NULL;
    bool foreground = false;
    int option;
    int result;

    while ((option = getopt(argc, argv, "C:fh")) != -1) {
        switch (option) {
        case 'C':
            config_directory = optarg;
            break;
        case 'f':
            foreground = true;
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

    log_init("sqm-mon", foreground);

    if (config_load(
            &config,
            config_directory,
            config_error,
            sizeof(config_error)
        ) != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "configuration error: %s",
            config_error
        );
        log_close();
        return 1;
    }

    (void)log_set_level(config.debug ? "debug" : "info");
    log_set_debug_syslog(config.log_debug_messages_to_syslog);

    if (!config.enabled) {
        log_message(LOG_LEVEL_NOTICE, "disabled by configuration");
        log_close();
        return 0;
    }

    if (config.log_file[0] != '\0' &&
        log_set_file(config.log_file) != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not open log file '%s': %s",
            config.log_file,
            strerror(errno)
        );
        log_close();
        return 1;
    }

    log_print_headers(
        config.output_processing_stats,
        config.output_load_stats,
        config.output_summary_stats
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
        config.log_file[0] == '\0' ? "disabled" : config.log_file
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
        "Starting sqm-mon with PID: %ld and config: /etc/config/sqm-mon",
        (long)getpid()
    );

    if (config.adjust_download || config.adjust_upload) {
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
        "Stopped sqm-mon with PID: %ld and config: /etc/config/sqm-mon",
        (long)getpid()
    );
    log_close();
    return result == 0 ? 0 : 1;
}
