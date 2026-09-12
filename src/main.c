#define _GNU_SOURCE

#include "cake.h"
#include "config.h"
#include "latency.h"
#include "log.h"
#include "netlink.h"
#include "traffic.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define SQM_MON_CAKE_ERROR_SIZE 256U
#define SQM_MON_CONFIG_ERROR_SIZE 256U
#define SQM_MON_LATENCY_ERROR_SIZE 256U
#define SQM_MON_LATENCY_TIMEOUT_MILLISECONDS 500
#define SQM_MON_TRAFFIC_ERROR_SIZE 256U
#define SQM_MON_TRAFFIC_INTERVAL_MILLISECONDS 1000

enum cake_observation_state {
    CAKE_OBSERVATION_UNKNOWN,
    CAKE_OBSERVATION_AVAILABLE,
    CAKE_OBSERVATION_NOT_FOUND,
    CAKE_OBSERVATION_FAILED
};

static void print_usage(const char *program_name)
{
    (void)fprintf(
        stderr,
        "Usage: %s [-f] [-C UCI_CONFIG_DIRECTORY]\n",
        program_name
    );
}

static void observe_traffic(
    struct sqm_mon_traffic_monitor *monitor,
    const char *interface,
    bool *read_failed
)
{
    struct traffic_sample sample;
    struct traffic_rates rates;
    char error[SQM_MON_TRAFFIC_ERROR_SIZE] = "";
    enum traffic_update_result update_result;

    if (traffic_read(
            interface,
            &sample,
            error,
            sizeof(error)
        ) != 0) {
        if (!*read_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "traffic observation degraded: interface=%s: %s",
                interface,
                error
            );
            traffic_monitor_init(monitor);
        }

        *read_failed = true;
        return;
    }

    if (*read_failed) {
        log_message(
            LOG_LEVEL_NOTICE,
            "traffic observation recovered: interface=%s",
            interface
        );
        *read_failed = false;
    }

    update_result = traffic_monitor_update(
        monitor,
        &sample,
        &rates
    );

    switch (update_result) {
    case TRAFFIC_UPDATE_BASELINE:
        log_message(
            LOG_LEVEL_INFO,
            "traffic observation initialized: interface=%s",
            interface
        );
        break;
    case TRAFFIC_UPDATE_RATES:
        log_message(
            LOG_LEVEL_DEBUG,
            "traffic: interface=%s rx_bytes=%" PRIu64
            " tx_bytes=%" PRIu64
            " rx_rate=%" PRIu64 " bit/s"
            " tx_rate=%" PRIu64 " bit/s",
            interface,
            sample.rx_bytes,
            sample.tx_bytes,
            rates.rx_bits_per_second,
            rates.tx_bits_per_second
        );
        break;
    case TRAFFIC_UPDATE_COUNTER_RESET:
        log_message(
            LOG_LEVEL_WARNING,
            "traffic counters reset: interface=%s",
            interface
        );
        break;
    case TRAFFIC_UPDATE_INVALID_INTERVAL:
        log_message(
            LOG_LEVEL_WARNING,
            "traffic sample interval was invalid: interface=%s",
            interface
        );
        break;
    }
}

static void observe_latency(
    struct sqm_mon_latency *latency,
    const struct sqm_mon_config *config,
    bool *observation_failed
)
{
    struct latency_sample sample;
    char error[SQM_MON_LATENCY_ERROR_SIZE] = "";
    enum latency_probe_result probe_result;

    if (latency->socket_descriptor < 0) {
        if (latency_open(
                latency,
                config->interface,
                config->latency_target,
                error,
                sizeof(error)
            ) != 0) {
            if (!*observation_failed) {
                log_message(
                    LOG_LEVEL_WARNING,
                    "latency observation degraded: target=%s: %s",
                    config->latency_target,
                    error
                );
            }

            *observation_failed = true;
            return;
        }

        log_message(
            *observation_failed ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO,
            *observation_failed
                ? "latency observation recovered: target=%s interface=%s"
                : "latency observation initialized: target=%s interface=%s",
            config->latency_target,
            config->interface
        );
        *observation_failed = false;
    }

    probe_result = latency_probe(
        latency,
        SQM_MON_LATENCY_TIMEOUT_MILLISECONDS,
        &sample,
        error,
        sizeof(error)
    );

    switch (probe_result) {
    case LATENCY_PROBE_SUCCESS:
        if (*observation_failed) {
            log_message(
                LOG_LEVEL_NOTICE,
                "latency observation recovered: target=%s",
                config->latency_target
            );
            *observation_failed = false;
        }

        log_message(
            LOG_LEVEL_DEBUG,
            "latency: target=%s rtt=%" PRIu32 " us",
            config->latency_target,
            sample.round_trip_microseconds
        );
        break;
    case LATENCY_PROBE_TIMEOUT:
        if (!*observation_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "latency observation degraded: target=%s: probe timed out",
                config->latency_target
            );
        }
        *observation_failed = true;
        break;
    case LATENCY_PROBE_ERROR:
        if (!*observation_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "latency observation degraded: target=%s: %s",
                config->latency_target,
                error
            );
        }
        *observation_failed = true;
        latency_close(latency);
        break;
    }
}

static void log_cake_discovery(
    const char *interface,
    const struct cake_observation *observation,
    bool recovered
)
{
    enum log_level level = recovered ? LOG_LEVEL_NOTICE : LOG_LEVEL_INFO;

    if (!observation->has_bandwidth ||
        observation->bandwidth_bits_per_second == 0U) {
        log_message(
            level,
            recovered
                ? "CAKE observation recovered: interface=%s handle=0x%08" PRIx32
                    " bandwidth=unlimited"
                : "CAKE discovered: interface=%s handle=0x%08" PRIx32
                    " bandwidth=unlimited",
            interface,
            observation->handle
        );
        return;
    }

    log_message(
        level,
        recovered
            ? "CAKE observation recovered: interface=%s handle=0x%08" PRIx32
                " bandwidth=%" PRIu64 " bit/s"
            : "CAKE discovered: interface=%s handle=0x%08" PRIx32
                " bandwidth=%" PRIu64 " bit/s",
        interface,
        observation->handle,
        observation->bandwidth_bits_per_second
    );
}

static void log_cake_sample(
    const char *interface,
    const struct cake_observation *observation
)
{
    log_message(
        LOG_LEVEL_DEBUG,
        "cake: interface=%s handle=0x%08" PRIx32
        " parent=0x%08" PRIx32
        " bandwidth=%" PRIu64 " bit/s"
        " capacity=%" PRIu64 " bit/s"
        " bytes=%" PRIu64 " packets=%" PRIu32
        " qlen=%" PRIu32 " backlog=%" PRIu32 " drops=%" PRIu32
        " memory_used=%" PRIu32 " memory_limit=%" PRIu32,
        interface,
        observation->handle,
        observation->parent,
        observation->bandwidth_bits_per_second,
        observation->capacity_estimate_bits_per_second,
        observation->bytes,
        observation->packets,
        observation->queue_length,
        observation->backlog_bytes,
        observation->drops,
        observation->memory_used_bytes,
        observation->memory_limit_bytes
    );
}

static void observe_cake(
    struct sqm_mon_netlink *netlink,
    const char *interface,
    enum cake_observation_state *state
)
{
    struct cake_observation observation;
    char error[SQM_MON_CAKE_ERROR_SIZE] = "";
    enum cake_read_result read_result;

    read_result = cake_read(
        netlink,
        interface,
        &observation,
        error,
        sizeof(error)
    );

    switch (read_result) {
    case CAKE_READ_FOUND:
        if (*state != CAKE_OBSERVATION_AVAILABLE) {
            log_cake_discovery(
                interface,
                &observation,
                *state != CAKE_OBSERVATION_UNKNOWN
            );
        }
        *state = CAKE_OBSERVATION_AVAILABLE;
        log_cake_sample(interface, &observation);
        break;
    case CAKE_READ_NOT_FOUND:
        if (*state != CAKE_OBSERVATION_NOT_FOUND) {
            log_message(
                *state == CAKE_OBSERVATION_AVAILABLE
                    ? LOG_LEVEL_WARNING
                    : LOG_LEVEL_INFO,
                *state == CAKE_OBSERVATION_AVAILABLE
                    ? "CAKE observation degraded: no CAKE qdisc found on interface=%s"
                    : "CAKE not found: interface=%s; observation will retry",
                interface
            );
        }
        *state = CAKE_OBSERVATION_NOT_FOUND;
        break;
    case CAKE_READ_ERROR:
        if (*state != CAKE_OBSERVATION_FAILED) {
            log_message(
                LOG_LEVEL_WARNING,
                "CAKE observation degraded: interface=%s: %s",
                interface,
                error
            );
        }
        *state = CAKE_OBSERVATION_FAILED;
        break;
    }
}

static int run_event_loop(
    int signal_file_descriptor,
    const struct sqm_mon_config *config
)
{
    struct pollfd descriptor = {
        .fd = signal_file_descriptor,
        .events = POLLIN,
        .revents = 0
    };
    struct sqm_mon_latency latency;
    struct sqm_mon_netlink netlink;
    struct sqm_mon_traffic_monitor traffic_monitor;
    enum cake_observation_state cake_state = CAKE_OBSERVATION_UNKNOWN;
    bool latency_observation_failed = false;
    bool traffic_read_failed = false;
    int result = -1;

    latency_init(&latency);
    netlink_init(&netlink);
    traffic_monitor_init(&traffic_monitor);
    observe_traffic(
        &traffic_monitor,
        config->interface,
        &traffic_read_failed
    );

    if (config->latency_target[0] == '\0') {
        log_message(
            LOG_LEVEL_INFO,
            "latency observation disabled: no target configured"
        );
    } else {
        observe_latency(
            &latency,
            config,
            &latency_observation_failed
        );
    }
    observe_cake(
        &netlink,
        config->interface,
        &cake_state
    );

    for (;;) {
        struct signalfd_siginfo signal_information;
        ssize_t bytes_read;
        int poll_result;

        poll_result = poll(
            &descriptor,
            1U,
            SQM_MON_TRAFFIC_INTERVAL_MILLISECONDS
        );
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            log_message(
                LOG_LEVEL_ERROR,
                "poll failed while waiting for shutdown: %s",
                strerror(errno)
            );
            goto done;
        }

        if (poll_result == 0) {
            observe_traffic(
                &traffic_monitor,
                config->interface,
                &traffic_read_failed
            );
            if (config->latency_target[0] != '\0') {
                observe_latency(
                    &latency,
                    config,
                    &latency_observation_failed
                );
            }
            observe_cake(
                &netlink,
                config->interface,
                &cake_state
            );
            continue;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            log_message(
                LOG_LEVEL_ERROR,
                "signal descriptor reported an error (revents=0x%x)",
                (unsigned int)descriptor.revents
            );
            goto done;
        }

        bytes_read = read(
            signal_file_descriptor,
            &signal_information,
            sizeof(signal_information)
        );
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }

            log_message(
                LOG_LEVEL_ERROR,
                "could not read shutdown signal: %s",
                strerror(errno)
            );
            goto done;
        }

        if ((size_t)bytes_read != sizeof(signal_information)) {
            log_message(
                LOG_LEVEL_ERROR,
                "received an incomplete shutdown signal"
            );
            goto done;
        }

        if (signal_information.ssi_signo == (uint32_t)SIGINT ||
            signal_information.ssi_signo == (uint32_t)SIGTERM) {
            log_message(
                LOG_LEVEL_NOTICE,
                "received signal %u; shutting down",
                signal_information.ssi_signo
            );
            result = 0;
            goto done;
        }
    }

done:
    netlink_close(&netlink);
    latency_close(&latency);
    return result;
}

static int create_signal_descriptor(sigset_t *previous_mask)
{
    sigset_t mask;
    int descriptor;

    if (sigemptyset(&mask) != 0 ||
        sigaddset(&mask, SIGINT) != 0 ||
        sigaddset(&mask, SIGTERM) != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not create shutdown signal mask: %s",
            strerror(errno)
        );
        return -1;
    }

    if (sigprocmask(SIG_BLOCK, &mask, previous_mask) != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not block shutdown signals: %s",
            strerror(errno)
        );
        return -1;
    }

    descriptor = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (descriptor < 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not create signal descriptor: %s",
            strerror(errno)
        );
        (void)sigprocmask(SIG_SETMASK, previous_mask, NULL);
        return -1;
    }

    return descriptor;
}

int main(int argc, char **argv)
{
    struct sqm_mon_config config;
    char config_error[SQM_MON_CONFIG_ERROR_SIZE] = "";
    const char *config_directory = NULL;
    sigset_t previous_mask;
    bool foreground = false;
    int signal_file_descriptor;
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

    if (log_set_level(config.log_level) != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "configuration error: invalid log level '%s'",
            config.log_level
        );
        log_close();
        return 1;
    }

    if (!config.enabled) {
        log_message(LOG_LEVEL_NOTICE, "disabled by configuration");
        log_close();
        return 0;
    }

    log_message(
        LOG_LEVEL_INFO,
        "configuration loaded: interface=%s latency_target=%s log_level=%s",
        config.interface,
        config.latency_target[0] == '\0'
            ? "disabled"
            : config.latency_target,
        config.log_level
    );

    signal_file_descriptor = create_signal_descriptor(&previous_mask);
    if (signal_file_descriptor < 0) {
        log_close();
        return 1;
    }

    log_message(LOG_LEVEL_NOTICE, "started in observation-only mode");
    result = run_event_loop(
        signal_file_descriptor,
        &config
    );

    if (close(signal_file_descriptor) != 0) {
        log_message(
            LOG_LEVEL_WARNING,
            "could not close signal descriptor: %s",
            strerror(errno)
        );
    }

    if (sigprocmask(SIG_SETMASK, &previous_mask, NULL) != 0) {
        log_message(
            LOG_LEVEL_WARNING,
            "could not restore signal mask: %s",
            strerror(errno)
        );
    }

    log_message(LOG_LEVEL_NOTICE, "stopped");
    log_close();
    return result == 0 ? 0 : 1;
}
