#define _GNU_SOURCE

#include "cake.h"
#include "config.h"
#include "controller.h"
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
#include <time.h>
#include <unistd.h>

#define SQM_MON_CAKE_ERROR_SIZE 256U
#define SQM_MON_CONFIG_ERROR_SIZE 256U
#define SQM_MON_LATENCY_ERROR_SIZE 256U
#define SQM_MON_LATENCY_TIMEOUT_MILLISECONDS 500
#define SQM_MON_TRAFFIC_INTERVAL_MILLISECONDS 1000

enum cake_observation_state {
    CAKE_OBSERVATION_UNKNOWN,
    CAKE_OBSERVATION_AVAILABLE,
    CAKE_OBSERVATION_NOT_FOUND,
    CAKE_OBSERVATION_FAILED
};

enum traffic_observation_state {
    TRAFFIC_OBSERVATION_UNKNOWN,
    TRAFFIC_OBSERVATION_AVAILABLE,
    TRAFFIC_OBSERVATION_UNAVAILABLE
};

static void print_usage(const char *program_name)
{
    (void)fprintf(
        stderr,
        "Usage: %s [-f] [-C UCI_CONFIG_DIRECTORY]\n",
        program_name
    );
}

static bool observe_traffic(
    struct sqm_mon_traffic_monitor *monitor,
    enum traffic_observation_state *state,
    const char *direction,
    const char *interface,
    const struct cake_observation *cake,
    bool cake_valid,
    const struct timespec *timestamp,
    uint64_t *rate_bits_per_second
)
{
    struct traffic_sample sample = {
        .bytes = cake_valid ? cake->bytes : 0U,
        .qdisc_handle = cake_valid ? cake->handle : 0U,
        .qdisc_parent = cake_valid ? cake->parent : 0U,
        .timestamp = *timestamp
    };
    enum traffic_update_result update_result;

    *rate_bits_per_second = 0U;
    if (!cake_valid || !cake->has_basic_stats) {
        if (*state != TRAFFIC_OBSERVATION_UNAVAILABLE) {
            log_message(
                LOG_LEVEL_WARNING,
                "traffic observation unavailable: direction=%s interface=%s"
                " source=CAKE basic stats",
                direction,
                interface
            );
        }
        *state = TRAFFIC_OBSERVATION_UNAVAILABLE;
        traffic_monitor_init(monitor);
        return false;
    }

    if (*state == TRAFFIC_OBSERVATION_UNAVAILABLE) {
        log_message(
            LOG_LEVEL_NOTICE,
            "traffic observation recovered: direction=%s interface=%s"
            " source=CAKE basic stats",
            direction,
            interface
        );
    }
    *state = TRAFFIC_OBSERVATION_AVAILABLE;

    update_result = traffic_monitor_update(
        monitor,
        &sample,
        rate_bits_per_second
    );

    switch (update_result) {
    case TRAFFIC_UPDATE_BASELINE:
        log_message(
            LOG_LEVEL_INFO,
            "traffic observation initialized: direction=%s interface=%s"
            " source=CAKE basic stats",
            direction,
            interface
        );
        break;
    case TRAFFIC_UPDATE_RATES:
        log_message(
            LOG_LEVEL_DEBUG,
            "traffic: direction=%s interface=%s source=cake"
            " bytes=%" PRIu64 " rate=%" PRIu64 " bit/s",
            direction,
            interface,
            sample.bytes,
            *rate_bits_per_second
        );
        return true;
    case TRAFFIC_UPDATE_COUNTER_RESET:
        log_message(
            LOG_LEVEL_WARNING,
            "CAKE traffic counter reset; re-baselining:"
            " direction=%s interface=%s",
            direction,
            interface
        );
        break;
    case TRAFFIC_UPDATE_QDISC_REPLACED:
        log_message(
            LOG_LEVEL_NOTICE,
            "CAKE qdisc changed; traffic observation re-baselined:"
            " direction=%s interface=%s handle=0x%08" PRIx32,
            direction,
            interface,
            sample.qdisc_handle
        );
        break;
    case TRAFFIC_UPDATE_INVALID_INTERVAL:
        log_message(
            LOG_LEVEL_WARNING,
            "traffic sample interval was invalid: direction=%s interface=%s",
            direction,
            interface
        );
        break;
    }

    return false;
}

static bool observe_latency(
    struct sqm_mon_latency *latency,
    struct latency_tracker *tracker,
    const struct sqm_mon_config *config,
    bool *observation_failed,
    struct latency_observation *observation
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
            return false;
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

        latency_tracker_update(
            tracker,
            &sample,
            observation
        );
        log_message(
            LOG_LEVEL_DEBUG,
            "latency: target=%s rtt=%" PRIu32 " us"
            " baseline=%" PRIu32 " us delta=%" PRIu32 " us",
            config->latency_target,
            observation->round_trip_microseconds,
            observation->baseline_microseconds,
            observation->delta_microseconds
        );
        return true;
    case LATENCY_PROBE_TIMEOUT:
        if (!*observation_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "latency observation degraded: target=%s: probe timed out",
                config->latency_target
            );
        }
        *observation_failed = true;
        return false;
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
        return false;
    }

    return false;
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

static bool observe_cake(
    struct sqm_mon_netlink *netlink,
    const char *interface,
    enum cake_observation_state *state,
    struct cake_observation *observation
)
{
    char error[SQM_MON_CAKE_ERROR_SIZE] = "";
    enum cake_read_result read_result;

    read_result = cake_read(
        netlink,
        interface,
        observation,
        error,
        sizeof(error)
    );

    switch (read_result) {
    case CAKE_READ_FOUND:
        if (*state != CAKE_OBSERVATION_AVAILABLE) {
            log_cake_discovery(
                interface,
                observation,
                *state != CAKE_OBSERVATION_UNKNOWN
            );
        }
        *state = CAKE_OBSERVATION_AVAILABLE;
        log_cake_sample(interface, observation);
        return true;
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

    return false;
}

struct observation_context {
    struct sqm_mon_controller controller;
    struct sqm_mon_latency latency;
    struct latency_tracker latency_tracker;
    struct sqm_mon_netlink netlink;
    struct sqm_mon_traffic_monitor download_traffic_monitor;
    struct sqm_mon_traffic_monitor upload_traffic_monitor;
    enum cake_observation_state ingress_cake_state;
    enum cake_observation_state upload_cake_state;
    enum traffic_observation_state download_traffic_state;
    enum traffic_observation_state upload_traffic_state;
    bool latency_observation_failed;
    bool traffic_clock_failed;
};

static const char *line_state_name(enum controller_line_state state)
{
    switch (state) {
    case CONTROLLER_LINE_UNKNOWN:
        return "unknown";
    case CONTROLLER_LINE_BELOW_CAPACITY:
        return "below-capacity";
    case CONTROLLER_LINE_SATURATED:
        return "saturated";
    }

    return "invalid";
}

static void log_line_state(
    const char *direction,
    enum controller_line_state state,
    const struct controller_direction_input *input
)
{
    if (state == CONTROLLER_LINE_UNKNOWN) {
        log_message(
            LOG_LEVEL_WARNING,
            "line load unavailable: direction=%s",
            direction
        );
        return;
    }

    log_message(
        state == CONTROLLER_LINE_SATURATED
            ? LOG_LEVEL_NOTICE
            : LOG_LEVEL_INFO,
        "line load changed: direction=%s state=%s"
        " traffic_rate=%" PRIu64 " bit/s"
        " cake_rate=%" PRIu64 " bit/s",
        direction,
        line_state_name(state),
        input->traffic_rate_bits_per_second,
        input->cake_rate_bits_per_second
    );
}

static const char *congestion_state_name(
    enum controller_congestion_state state
)
{
    switch (state) {
    case CONTROLLER_CONGESTION_UNKNOWN:
        return "unknown";
    case CONTROLLER_CONGESTION_CLEAR:
        return "clear";
    case CONTROLLER_CONGESTION_DETECTED:
        return "detected";
    }

    return "invalid";
}

static void log_congestion_state(
    const char *direction,
    enum controller_congestion_state state,
    const struct controller_latency_input *latency
)
{
    uint32_t delay;

    if (state == CONTROLLER_CONGESTION_UNKNOWN) {
        log_message(
            LOG_LEVEL_WARNING,
            "congestion observation unavailable: direction=%s",
            direction
        );
        return;
    }

    delay = latency->current_rtt_microseconds >=
            latency->baseline_rtt_microseconds
        ? latency->current_rtt_microseconds -
            latency->baseline_rtt_microseconds
        : 0U;
    log_message(
        state == CONTROLLER_CONGESTION_DETECTED
            ? LOG_LEVEL_NOTICE
            : LOG_LEVEL_INFO,
        "congestion changed: direction=%s state=%s"
        " rtt=%" PRIu32 " us baseline=%" PRIu32 " us"
        " delta=%" PRIu32 " us",
        direction,
        congestion_state_name(state),
        latency->current_rtt_microseconds,
        latency->baseline_rtt_microseconds,
        delay
    );
}

static const char *rate_reason_name(enum controller_rate_reason reason)
{
    switch (reason) {
    case CONTROLLER_RATE_UNCHANGED:
        return "unchanged";
    case CONTROLLER_RATE_INITIAL:
        return "initial";
    case CONTROLLER_RATE_CONGESTION:
        return "congestion";
    case CONTROLLER_RATE_HIGH_LOAD:
        return "high-load";
    case CONTROLLER_RATE_RETURN_TO_BASE:
        return "return-to-base";
    case CONTROLLER_RATE_RECONCILE:
        return "reconcile";
    }

    return "invalid";
}

static void apply_bandwidth(
    struct sqm_mon_netlink *netlink,
    const char *direction,
    const char *interface,
    const struct cake_observation *current,
    uint64_t desired_rate,
    enum controller_rate_reason reason
)
{
    struct cake_observation verified;
    char error[SQM_MON_CAKE_ERROR_SIZE] = "";
    enum cake_read_result read_result;

    if (cake_set_bandwidth(
            netlink,
            interface,
            current,
            desired_rate,
            error,
            sizeof(error)
        ) != 0) {
        log_message(
            LOG_LEVEL_WARNING,
            "CAKE bandwidth change failed: direction=%s interface=%s"
            " old_rate=%" PRIu64 " bit/s desired_rate=%" PRIu64
            " bit/s reason=%s: %s",
            direction,
            interface,
            current->bandwidth_bits_per_second,
            desired_rate,
            rate_reason_name(reason),
            error
        );
        return;
    }

    read_result = cake_read(
        netlink,
        interface,
        &verified,
        error,
        sizeof(error)
    );
    if (read_result != CAKE_READ_FOUND || !verified.has_bandwidth ||
        verified.bandwidth_bits_per_second != desired_rate) {
        log_message(
            LOG_LEVEL_WARNING,
            "CAKE bandwidth verification failed: direction=%s interface=%s"
            " desired_rate=%" PRIu64 " bit/s result=%s",
            direction,
            interface,
            desired_rate,
            read_result == CAKE_READ_ERROR
                ? error
                : "readback did not match"
        );
        return;
    }

    log_message(
        LOG_LEVEL_NOTICE,
        "CAKE bandwidth changed: direction=%s interface=%s"
        " old_rate=%" PRIu64 " bit/s new_rate=%" PRIu64
        " bit/s reason=%s",
        direction,
        interface,
        current->bandwidth_bits_per_second,
        verified.bandwidth_bits_per_second,
        rate_reason_name(reason)
    );
}

static void update_controller(
    struct sqm_mon_controller *controller,
    struct sqm_mon_netlink *netlink,
    const struct sqm_mon_config *config,
    const struct traffic_rates *rates,
    const struct cake_observation *ingress_cake,
    bool ingress_cake_valid,
    const struct cake_observation *upload_cake,
    bool upload_cake_valid,
    const struct latency_observation *latency,
    bool latency_valid
)
{
    struct timespec current_time;
    struct controller_input input = {
        .download = {
            .valid = rates->download_valid &&
                ingress_cake_valid &&
                ingress_cake->has_bandwidth &&
                ingress_cake->bandwidth_bits_per_second > 0U,
            .traffic_rate_bits_per_second =
                rates->download_bits_per_second,
            .cake_rate_bits_per_second = ingress_cake_valid
                ? ingress_cake->bandwidth_bits_per_second
                : 0U
        },
        .upload = {
            .valid = rates->upload_valid &&
                upload_cake_valid &&
                upload_cake->has_bandwidth &&
                upload_cake->bandwidth_bits_per_second > 0U,
            .traffic_rate_bits_per_second = rates->upload_bits_per_second,
            .cake_rate_bits_per_second = upload_cake_valid
                ? upload_cake->bandwidth_bits_per_second
                : 0U
        },
        .latency = {
            .valid = latency_valid,
            .current_rtt_microseconds = latency_valid
                ? latency->round_trip_microseconds
                : 0U,
            .baseline_rtt_microseconds = latency_valid
                ? latency->baseline_microseconds
                : 0U
        },
        .timestamp_microseconds = 0U
    };
    struct controller_output output;

    if (clock_gettime(CLOCK_MONOTONIC, &current_time) == 0 &&
        current_time.tv_sec >= 0) {
        input.timestamp_microseconds =
            (uint64_t)current_time.tv_sec * 1000000U +
            (uint64_t)current_time.tv_nsec / 1000U;
    }

    controller_update(controller, &input, &output);
    if (output.download_state_changed) {
        log_line_state("download", output.download_state, &input.download);
    }
    if (output.upload_state_changed) {
        log_line_state("upload", output.upload_state, &input.upload);
    }
    if (output.download_congestion_changed) {
        log_congestion_state(
            "download",
            output.download_congestion,
            &input.latency
        );
    }
    if (output.upload_congestion_changed) {
        log_congestion_state(
            "upload",
            output.upload_congestion,
            &input.latency
        );
    }
    if (config->adjust_download && output.download_rate_changed) {
        apply_bandwidth(
            netlink,
            "download",
            config->ingress_interface,
            ingress_cake,
            output.download_rate_bits_per_second,
            output.download_rate_reason
        );
    }
    if (config->adjust_upload && output.upload_rate_changed) {
        apply_bandwidth(
            netlink,
            "upload",
            config->interface,
            upload_cake,
            output.upload_rate_bits_per_second,
            output.upload_rate_reason
        );
    }
}

static void observe_cycle(
    struct observation_context *context,
    const struct sqm_mon_config *config
)
{
    struct cake_observation ingress_cake;
    struct cake_observation upload_cake;
    struct latency_observation latency = { 0U, 0U, 0U };
    struct traffic_rates rates = { 0U, 0U, false, false };
    struct timespec traffic_timestamp;
    bool ingress_cake_valid = false;
    bool latency_valid = false;
    bool upload_cake_valid;

    if (config->latency_target[0] != '\0') {
        latency_valid = observe_latency(
            &context->latency,
            &context->latency_tracker,
            config,
            &context->latency_observation_failed,
            &latency
        );
    }
    upload_cake_valid = observe_cake(
        &context->netlink,
        config->interface,
        &context->upload_cake_state,
        &upload_cake
    );
    if (config->ingress_interface[0] != '\0') {
        ingress_cake_valid = observe_cake(
            &context->netlink,
            config->ingress_interface,
            &context->ingress_cake_state,
            &ingress_cake
        );
    }
    if (clock_gettime(CLOCK_MONOTONIC, &traffic_timestamp) != 0) {
        if (!context->traffic_clock_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "traffic observation degraded: monotonic clock failed: %s",
                strerror(errno)
            );
        }
        context->traffic_clock_failed = true;
        traffic_monitor_init(&context->download_traffic_monitor);
        traffic_monitor_init(&context->upload_traffic_monitor);
    } else {
        if (context->traffic_clock_failed) {
            log_message(
                LOG_LEVEL_NOTICE,
                "traffic observation recovered: monotonic clock available"
            );
            context->traffic_clock_failed = false;
        }
        if (config->ingress_interface[0] != '\0') {
            rates.download_valid = observe_traffic(
                &context->download_traffic_monitor,
                &context->download_traffic_state,
                "download",
                config->ingress_interface,
                &ingress_cake,
                ingress_cake_valid,
                &traffic_timestamp,
                &rates.download_bits_per_second
            );
        }
        rates.upload_valid = observe_traffic(
            &context->upload_traffic_monitor,
            &context->upload_traffic_state,
            "upload",
            config->interface,
            &upload_cake,
            upload_cake_valid,
            &traffic_timestamp,
            &rates.upload_bits_per_second
        );
    }
    update_controller(
        &context->controller,
        &context->netlink,
        config,
        &rates,
        &ingress_cake,
        ingress_cake_valid,
        &upload_cake,
        upload_cake_valid,
        &latency,
        latency_valid
    );
}

static int run_event_loop(
    int signal_file_descriptor,
    const struct sqm_mon_config *config
)
{
    const struct controller_config controller_config = {
        .download = {
            .adjust = config->adjust_download,
            .minimum_rate_bits_per_second =
                config->minimum_download_rate_bits_per_second,
            .base_rate_bits_per_second =
                config->base_download_rate_bits_per_second,
            .maximum_rate_bits_per_second =
                config->maximum_download_rate_bits_per_second
        },
        .upload = {
            .adjust = config->adjust_upload,
            .minimum_rate_bits_per_second =
                config->minimum_upload_rate_bits_per_second,
            .base_rate_bits_per_second =
                config->base_upload_rate_bits_per_second,
            .maximum_rate_bits_per_second =
                config->maximum_upload_rate_bits_per_second
        }
    };
    struct pollfd descriptor = {
        .fd = signal_file_descriptor,
        .events = POLLIN,
        .revents = 0
    };
    struct observation_context context = {
        .ingress_cake_state = CAKE_OBSERVATION_UNKNOWN,
        .upload_cake_state = CAKE_OBSERVATION_UNKNOWN,
        .download_traffic_state = TRAFFIC_OBSERVATION_UNKNOWN,
        .upload_traffic_state = TRAFFIC_OBSERVATION_UNKNOWN,
        .latency_observation_failed = false,
        .traffic_clock_failed = false
    };
    int result = -1;

    controller_init(&context.controller, &controller_config);
    latency_init(&context.latency);
    latency_tracker_init(&context.latency_tracker);
    netlink_init(&context.netlink);
    traffic_monitor_init(&context.download_traffic_monitor);
    traffic_monitor_init(&context.upload_traffic_monitor);

    if (config->latency_target[0] == '\0') {
        log_message(
            LOG_LEVEL_INFO,
            "latency observation disabled: no target configured"
        );
    }
    if (config->ingress_interface[0] == '\0') {
        log_message(
            LOG_LEVEL_INFO,
            "download line detection disabled: no ingress interface configured"
        );
    }
    observe_cycle(&context, config);

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
            observe_cycle(&context, config);
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
    netlink_close(&context.netlink);
    latency_close(&context.latency);
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

    log_message(
        LOG_LEVEL_INFO,
        "configuration loaded: upload_interface=%s download_interface=%s"
        " latency_target=%s log_level=%s log_file=%s",
        config.interface,
        config.ingress_interface[0] == '\0'
            ? "disabled"
            : config.ingress_interface,
        config.latency_target[0] == '\0'
            ? "disabled"
            : config.latency_target,
        config.log_level,
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

    signal_file_descriptor = create_signal_descriptor(&previous_mask);
    if (signal_file_descriptor < 0) {
        log_close();
        return 1;
    }

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
