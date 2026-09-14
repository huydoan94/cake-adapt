#define _GNU_SOURCE

#include "monitor.h"

#include "cake.h"
#include "controller.h"
#include "latency.h"
#include "log.h"
#include "netlink.h"
#include "traffic.h"

#include <libubox/uloop.h>

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ERROR_SIZE 256U
#define LOAD_CONDITION_SIZE 16U

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

struct monitored_direction {
    const char *name;
    const char *interface;
    struct sqm_mon_traffic_monitor traffic_monitor;
    struct cake_observation cake;
    enum cake_observation_state cake_state;
    enum traffic_observation_state traffic_state;
    uint64_t traffic_rate_bits_per_second;
    bool cake_valid;
    bool traffic_valid;
};

static void observe_traffic(
    struct monitored_direction *direction,
    const struct timespec *timestamp
)
{
    struct traffic_sample sample = {
        .bytes = direction->cake_valid ? direction->cake.bytes : 0U,
        .qdisc_handle = direction->cake_valid ? direction->cake.handle : 0U,
        .qdisc_parent = direction->cake_valid ? direction->cake.parent : 0U,
        .timestamp = *timestamp
    };
    enum traffic_update_result update_result;

    direction->traffic_rate_bits_per_second = 0U;
    direction->traffic_valid = false;
    if (!direction->cake_valid || !direction->cake.has_basic_stats) {
        if (direction->traffic_state != TRAFFIC_OBSERVATION_UNAVAILABLE) {
            log_message(
                LOG_LEVEL_WARNING,
                "traffic observation unavailable: direction=%s interface=%s"
                " source=CAKE basic stats",
                direction->name,
                direction->interface
            );
        }
        direction->traffic_state = TRAFFIC_OBSERVATION_UNAVAILABLE;
        traffic_monitor_init(&direction->traffic_monitor);
        return;
    }

    if (direction->traffic_state == TRAFFIC_OBSERVATION_UNAVAILABLE) {
        log_message(
            LOG_LEVEL_NOTICE,
            "traffic observation recovered: direction=%s interface=%s"
            " source=CAKE basic stats",
            direction->name,
            direction->interface
        );
    }
    direction->traffic_state = TRAFFIC_OBSERVATION_AVAILABLE;

    update_result = traffic_monitor_update(
        &direction->traffic_monitor,
        &sample,
        &direction->traffic_rate_bits_per_second
    );

    switch (update_result) {
    case TRAFFIC_UPDATE_BASELINE:
        log_message(
            LOG_LEVEL_INFO,
            "traffic observation initialized: direction=%s interface=%s"
            " source=CAKE basic stats",
            direction->name,
            direction->interface
        );
        break;
    case TRAFFIC_UPDATE_RATES:
        direction->traffic_valid = true;
        return;
    case TRAFFIC_UPDATE_COUNTER_RESET:
        log_message(
            LOG_LEVEL_WARNING,
            "CAKE traffic counter reset; re-baselining:"
            " direction=%s interface=%s",
            direction->name,
            direction->interface
        );
        break;
    case TRAFFIC_UPDATE_QDISC_REPLACED:
        log_message(
            LOG_LEVEL_NOTICE,
            "CAKE qdisc changed; traffic observation re-baselined:"
            " direction=%s interface=%s handle=0x%08" PRIx32,
            direction->name,
            direction->interface,
            sample.qdisc_handle
        );
        break;
    case TRAFFIC_UPDATE_INVALID_INTERVAL:
        log_message(
            LOG_LEVEL_WARNING,
            "traffic sample interval was invalid: direction=%s interface=%s",
            direction->name,
            direction->interface
        );
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
    struct monitored_direction *direction
)
{
    char error[ERROR_SIZE] = "";
    enum cake_read_result read_result;

    read_result = cake_read(
        netlink,
        direction->interface,
        &direction->cake,
        error,
        sizeof(error)
    );
    direction->cake_valid = false;

    switch (read_result) {
    case CAKE_READ_FOUND:
        if (direction->cake_state != CAKE_OBSERVATION_AVAILABLE) {
            log_cake_discovery(
                direction->interface,
                &direction->cake,
                direction->cake_state != CAKE_OBSERVATION_UNKNOWN
            );
        }
        direction->cake_state = CAKE_OBSERVATION_AVAILABLE;
        direction->cake_valid = true;
        log_cake_sample(direction->interface, &direction->cake);
        return;
    case CAKE_READ_NOT_FOUND:
        if (direction->cake_state != CAKE_OBSERVATION_NOT_FOUND) {
            log_message(
                direction->cake_state == CAKE_OBSERVATION_AVAILABLE
                    ? LOG_LEVEL_WARNING
                    : LOG_LEVEL_INFO,
                direction->cake_state == CAKE_OBSERVATION_AVAILABLE
                    ? "CAKE observation degraded: no CAKE qdisc found on interface=%s"
                    : "CAKE not found: interface=%s; observation will retry",
                direction->interface
            );
        }
        direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
        break;
    case CAKE_READ_ERROR:
        if (direction->cake_state != CAKE_OBSERVATION_FAILED) {
            log_message(
                LOG_LEVEL_WARNING,
                "CAKE observation degraded: interface=%s: %s",
                direction->interface,
                error
            );
        }
        direction->cake_state = CAKE_OBSERVATION_FAILED;
        break;
    }
}

struct observation_context {
    struct sqm_mon_controller controller;
    struct sqm_mon_latency latency;
    struct latency_tracker latency_trackers[CONFIG_MAX_REFLECTORS];
    struct reflector_health reflector_health[CONFIG_MAX_REFLECTORS];
    struct sqm_mon_netlink netlink;
    struct monitored_direction download;
    struct monitored_direction upload;
    bool latency_observation_failed;
    bool traffic_clock_failed;
    bool health_clock_failed;
};

struct event_loop {
    struct observation_context observation;
    const struct sqm_mon_config *config;
    struct uloop_interval traffic_timer;
    struct uloop_interval reflector_health_timer;
    struct uloop_fd latency_output;
    int result;
};

static bool monotonic_microseconds(uint64_t *timestamp_microseconds)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0 ||
        timestamp.tv_sec < 0) {
        return false;
    }
    *timestamp_microseconds =
        (uint64_t)timestamp.tv_sec * 1000000U +
        (uint64_t)timestamp.tv_nsec / 1000U;
    return true;
}

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

static uint64_t rounded_divide(uint64_t value, uint64_t divisor)
{
    uint64_t quotient = value / divisor;
    uint64_t remainder = value % divisor;

    return quotient + (remainder >= (divisor + 1U) / 2U ? 1U : 0U);
}

static bool direction_has_low_load(
    const struct monitored_direction *direction,
    uint64_t high_load_threshold_percent
)
{
    return direction->traffic_valid &&
        controller_load_percent(
            direction->traffic_rate_bits_per_second,
            direction->cake_valid
                ? direction->cake.bandwidth_bits_per_second
                : 0U
        ) < high_load_threshold_percent;
}

static void load_condition(
    char *condition,
    size_t condition_size,
    const char *direction,
    uint64_t traffic_rate,
    uint64_t cake_rate,
    uint64_t connection_active_threshold,
    uint64_t high_load_threshold_percent,
    enum controller_congestion_state congestion
)
{
    const char *state;

    if (controller_load_percent(traffic_rate, cake_rate) >
        high_load_threshold_percent) {
        state = "high";
    } else if (traffic_rate > connection_active_threshold) {
        state = "low";
    } else {
        state = "idle";
    }

    (void)snprintf(
        condition,
        condition_size,
        "%s_%s%s",
        direction,
        state,
        congestion == CONTROLLER_CONGESTION_DETECTED ? "_bb" : ""
    );
}

static void log_load_stats(
    const struct monitored_direction *download,
    const struct monitored_direction *upload
)
{
    const struct log_load_record record = {
        .download_achieved_rate_kbps =
            download->traffic_rate_bits_per_second / 1000U,
        .upload_achieved_rate_kbps =
            upload->traffic_rate_bits_per_second / 1000U,
        .cake_download_rate_kbps =
            download->cake.bandwidth_bits_per_second / 1000U,
        .cake_upload_rate_kbps =
            upload->cake.bandwidth_bits_per_second / 1000U
    };

    log_load(&record);
}

static void log_controller_stats(
    const struct sqm_mon_config *config,
    const struct controller_input *input,
    const struct controller_output *output,
    const struct latency_observation *latency,
    const char *reflector
)
{
    char download_condition[LOAD_CONDITION_SIZE];
    char upload_condition[LOAD_CONDITION_SIZE];
    uint64_t download_rate = output->download.rate_bits_per_second / 1000U;
    uint64_t upload_rate = output->upload.rate_bits_per_second / 1000U;

    load_condition(
        download_condition,
        sizeof(download_condition),
        "dl",
        input->download.traffic_rate_bits_per_second,
        input->download.cake_rate_bits_per_second,
        config->connection_active_threshold_bits_per_second,
        rounded_divide(
            config->high_load_threshold_per_million,
            10000U
        ),
        output->download.congestion
    );
    load_condition(
        upload_condition,
        sizeof(upload_condition),
        "ul",
        input->upload.traffic_rate_bits_per_second,
        input->upload.cake_rate_bits_per_second,
        config->connection_active_threshold_bits_per_second,
        rounded_divide(
            config->high_load_threshold_per_million,
            10000U
        ),
        output->upload.congestion
    );

    if (config->output_processing_stats) {
        /*
         * Standard fping supplies RTT rather than directional timestamps.
         * Match cake-autorate's fping path by recording the same half-RTT
         * estimate in its separate download and upload OWD columns.
         */
        const struct log_data_record record = {
            .download_achieved_rate_kbps =
                input->download.traffic_rate_bits_per_second / 1000U,
            .upload_achieved_rate_kbps =
                input->upload.traffic_rate_bits_per_second / 1000U,
            .download_load_percent = controller_load_percent(
                input->download.traffic_rate_bits_per_second,
                input->download.cake_rate_bits_per_second
            ),
            .upload_load_percent = controller_load_percent(
                input->upload.traffic_rate_bits_per_second,
                input->upload.cake_rate_bits_per_second
            ),
            .icmp_timestamp_microseconds = latency->timestamp_microseconds,
            .reflector = reflector,
            .sequence = latency->sequence,
            .download_owd_baseline_microseconds =
                latency->one_way_baseline_microseconds,
            .download_owd_microseconds = latency->one_way_microseconds,
            .download_owd_delta_ewma_microseconds =
                latency->one_way_delta_ewma_microseconds,
            .download_owd_delta_microseconds =
                latency->one_way_delta_microseconds,
            .download_adjust_delay_threshold_microseconds =
                config->download_owd_delta_delay_threshold_microseconds,
            .upload_owd_baseline_microseconds =
                latency->one_way_baseline_microseconds,
            .upload_owd_microseconds = latency->one_way_microseconds,
            .upload_owd_delta_ewma_microseconds =
                latency->one_way_delta_ewma_microseconds,
            .upload_owd_delta_microseconds =
                latency->one_way_delta_microseconds,
            .upload_adjust_delay_threshold_microseconds =
                config->upload_owd_delta_delay_threshold_microseconds,
            .download_sum_delays =
                output->download.delayed_sample_count,
            .download_average_owd_delta_microseconds =
                output->download.average_delay_microseconds,
            .download_maximum_adjust_up_threshold_microseconds =
                config->download_average_owd_delta_maximum_adjust_up_microseconds,
            .download_maximum_adjust_down_threshold_microseconds =
                config->download_average_owd_delta_maximum_adjust_down_microseconds,
            .upload_sum_delays = output->upload.delayed_sample_count,
            .upload_average_owd_delta_microseconds =
                output->upload.average_delay_microseconds,
            .upload_maximum_adjust_up_threshold_microseconds =
                config->upload_average_owd_delta_maximum_adjust_up_microseconds,
            .upload_maximum_adjust_down_threshold_microseconds =
                config->upload_average_owd_delta_maximum_adjust_down_microseconds,
            .download_load_condition = download_condition,
            .upload_load_condition = upload_condition,
            .cake_download_rate_kbps = download_rate,
            .cake_upload_rate_kbps = upload_rate
        };

        log_data(&record);
    }

    if (config->output_summary_stats) {
        const struct log_summary_record record = {
            .download_achieved_rate_kbps =
                input->download.traffic_rate_bits_per_second / 1000U,
            .upload_achieved_rate_kbps =
                input->upload.traffic_rate_bits_per_second / 1000U,
            .download_sum_delays =
                output->download.delayed_sample_count,
            .upload_sum_delays = output->upload.delayed_sample_count,
            .download_average_owd_delta_microseconds =
                output->download.average_delay_microseconds,
            .upload_average_owd_delta_microseconds =
                output->upload.average_delay_microseconds,
            .download_load_condition = download_condition,
            .upload_load_condition = upload_condition,
            .cake_download_rate_kbps = download_rate,
            .cake_upload_rate_kbps = upload_rate
        };

        log_summary(&record);
    }
}

static void apply_bandwidth(
    struct sqm_mon_netlink *netlink,
    struct monitored_direction *direction,
    uint64_t desired_rate,
    enum controller_rate_reason reason,
    bool output_cake_changes
)
{
    struct cake_observation verified;
    char error[ERROR_SIZE] = "";
    enum cake_read_result read_result;

    if (output_cake_changes) {
        log_shaper(direction->interface, desired_rate / 1000U);
    }

    if (cake_set_bandwidth(
            netlink,
            direction->interface,
            &direction->cake,
            desired_rate,
            error,
            sizeof(error)
        ) != 0) {
        log_message(
            LOG_LEVEL_WARNING,
            "CAKE bandwidth change failed: direction=%s interface=%s"
            " old_rate=%" PRIu64 " bit/s desired_rate=%" PRIu64
            " bit/s reason=%s: %s",
            direction->name,
            direction->interface,
            direction->cake.bandwidth_bits_per_second,
            desired_rate,
            rate_reason_name(reason),
            error
        );
        return;
    }

    read_result = cake_read(
        netlink,
        direction->interface,
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
            direction->name,
            direction->interface,
            desired_rate,
            read_result == CAKE_READ_ERROR
                ? error
                : "readback did not match"
        );
        return;
    }

    direction->cake = verified;
}

static struct controller_direction_input direction_input(
    const struct monitored_direction *direction
)
{
    const struct controller_direction_input input = {
        .valid = direction->traffic_valid &&
            direction->cake_valid &&
            direction->cake.has_bandwidth &&
            direction->cake.bandwidth_bits_per_second > 0U,
        .traffic_rate_bits_per_second =
            direction->traffic_rate_bits_per_second,
        .cake_rate_bits_per_second = direction->cake_valid
            ? direction->cake.bandwidth_bits_per_second
            : 0U
    };

    return input;
}

static void update_controller(
    struct sqm_mon_controller *controller,
    struct sqm_mon_netlink *netlink,
    const struct sqm_mon_config *config,
    struct monitored_direction *download,
    struct monitored_direction *upload,
    const struct latency_observation *latency,
    bool latency_valid,
    const char *reflector
)
{
    struct timespec current_time;
    struct controller_input input = {
        .download = direction_input(download),
        .upload = direction_input(upload),
        .latency = {
            .valid = latency_valid,
            .current_rtt_microseconds = latency_valid
                ? latency->one_way_microseconds * 2U
                : 0U,
            .baseline_rtt_microseconds = latency_valid
                ? latency->one_way_baseline_microseconds * 2U
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
    if (output.download.state_changed) {
        log_line_state(
            download->name,
            output.download.state,
            &input.download
        );
    }
    if (output.upload.state_changed) {
        log_line_state(upload->name, output.upload.state, &input.upload);
    }
    if (output.download.congestion_changed) {
        log_congestion_state(
            download->name,
            output.download.congestion,
            &input.latency
        );
    }
    if (output.upload.congestion_changed) {
        log_congestion_state(
            upload->name,
            output.upload.congestion,
            &input.latency
        );
    }
    if (config->adjust_download && output.download.rate_changed) {
        apply_bandwidth(
            netlink,
            download,
            output.download.rate_bits_per_second,
            output.download.rate_reason,
            config->output_cake_changes
        );
    }
    if (config->adjust_upload && output.upload.rate_changed) {
        apply_bandwidth(
            netlink,
            upload,
            output.upload.rate_bits_per_second,
            output.upload.rate_reason,
            config->output_cake_changes
        );
    }
    if (latency_valid) {
        log_controller_stats(config, &input, &output, latency, reflector);
    }
}

static void observe_traffic_cycle(
    struct observation_context *context,
    const struct sqm_mon_config *config
)
{
    struct timespec traffic_timestamp;

    observe_cake(&context->netlink, &context->upload);
    observe_cake(&context->netlink, &context->download);
    if (clock_gettime(CLOCK_MONOTONIC, &traffic_timestamp) != 0) {
        if (!context->traffic_clock_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "traffic observation degraded: monotonic clock failed: %s",
                strerror(errno)
            );
        }
        context->traffic_clock_failed = true;
        context->download.traffic_valid = false;
        context->upload.traffic_valid = false;
        traffic_monitor_init(&context->download.traffic_monitor);
        traffic_monitor_init(&context->upload.traffic_monitor);
    } else {
        if (context->traffic_clock_failed) {
            log_message(
                LOG_LEVEL_NOTICE,
                "traffic observation recovered: monotonic clock available"
            );
            context->traffic_clock_failed = false;
        }
        observe_traffic(&context->download, &traffic_timestamp);
        observe_traffic(&context->upload, &traffic_timestamp);
    }

    if (config->output_load_stats &&
        context->download.traffic_valid && context->upload.traffic_valid &&
        context->download.cake_valid && context->upload.cake_valid &&
        context->download.cake.has_bandwidth &&
        context->upload.cake.has_bandwidth) {
        log_load_stats(&context->download, &context->upload);
    }
}

static bool ensure_latency_open(
    struct observation_context *context,
    const struct sqm_mon_config *config
)
{
    const char *targets[CONFIG_MAX_REFLECTORS];
    char error[ERROR_SIZE] = "";
    size_t target_count = (size_t)config->no_pingers;
    size_t index;

    if (latency_is_open(&context->latency)) {
        return true;
    }
    for (index = 0U; index < target_count; index++) {
        targets[index] = config->reflectors[index];
    }
    if (latency_open(
            &context->latency,
            config->interface,
            targets,
            target_count,
            config->reflector_ping_interval_microseconds,
            error,
            sizeof(error)
        ) != 0) {
        if (!context->latency_observation_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "latency observation degraded: %s",
                error
            );
        }
        context->latency_observation_failed = true;
        return false;
    }

    log_message(
        context->latency_observation_failed
            ? LOG_LEVEL_NOTICE
            : LOG_LEVEL_INFO,
        context->latency_observation_failed
            ? "latency observation recovered: targets=%zu interface=%s"
            : "latency observation initialized: targets=%zu interface=%s",
        target_count,
        config->interface
    );
    context->latency_observation_failed = false;
    return true;
}

static size_t find_active_reflector(
    const struct sqm_mon_config *config,
    const char *target
)
{
    size_t target_count = (size_t)config->no_pingers;
    size_t index;

    for (index = 0U; index < target_count; index++) {
        if (strcmp(config->reflectors[index], target) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool receive_latency_samples(
    struct observation_context *context,
    const struct sqm_mon_config *config
)
{
    for (;;) {
        struct latency_observation observation;
        struct latency_sample sample;
        char error[ERROR_SIZE] = "";
        enum latency_probe_result result = latency_receive(
            &context->latency,
            &sample,
            error,
            sizeof(error)
        );
        size_t reflector_index;
        bool low_load;

        if (result == LATENCY_PROBE_PENDING) {
            return true;
        }
        if (result == LATENCY_PROBE_TIMEOUT) {
            continue;
        }
        if (result == LATENCY_PROBE_ERROR) {
            if (!context->latency_observation_failed) {
                log_message(
                    LOG_LEVEL_WARNING,
                    "latency observation degraded: %s",
                    error
                );
            }
            context->latency_observation_failed = true;
            return false;
        }

        reflector_index = find_active_reflector(config, sample.target);
        if (reflector_index == SIZE_MAX) {
            log_message(
                LOG_LEVEL_WARNING,
                "latency observation degraded: unexpected reflector=%s",
                sample.target
            );
            context->latency_observation_failed = true;
            return false;
        }

        latency_tracker_update(
            &context->latency_trackers[reflector_index],
            &sample,
            &observation
        );
        {
            uint64_t response_timestamp_microseconds;

            if (monotonic_microseconds(&response_timestamp_microseconds)) {
                reflector_health_record_response(
                    &context->reflector_health[reflector_index],
                    response_timestamp_microseconds
                );
            }
        }
        low_load = direction_has_low_load(
            &context->download,
            rounded_divide(
                config->high_load_threshold_per_million,
                10000U
            )
        ) && direction_has_low_load(
            &context->upload,
            rounded_divide(
                config->high_load_threshold_per_million,
                10000U
            )
        );

        latency_tracker_update_delta_ewma(
            &context->latency_trackers[reflector_index],
            low_load,
            &observation
        );
        update_controller(
            &context->controller,
            &context->netlink,
            config,
            &context->download,
            &context->upload,
            &observation,
            true,
            sample.target
        );
    }
}

static struct event_loop *event_loop_from_latency_output(
    struct uloop_fd *descriptor
)
{
    return (struct event_loop *)(void *)(
        (unsigned char *)(void *)descriptor -
        offsetof(struct event_loop, latency_output)
    );
}

static void close_latency(struct event_loop *loop)
{
    if (loop->latency_output.registered) {
        (void)uloop_fd_delete(&loop->latency_output);
    }
    loop->latency_output.fd = -1;
    latency_close(&loop->observation.latency);
}

static void handle_latency_output(
    struct uloop_fd *descriptor,
    unsigned int events
)
{
    struct event_loop *loop = event_loop_from_latency_output(descriptor);

    (void)events;
    if (!receive_latency_samples(&loop->observation, loop->config)) {
        close_latency(loop);
    }
}

static bool watch_latency(struct event_loop *loop)
{
    int saved_errno;

    if (loop->latency_output.registered) {
        return true;
    }
    if (!ensure_latency_open(&loop->observation, loop->config)) {
        return false;
    }

    loop->latency_output.fd = loop->observation.latency.output_descriptor;
    if (uloop_fd_add(
            &loop->latency_output,
            ULOOP_READ | ULOOP_ERROR_CB
        ) == 0) {
        return true;
    }

    saved_errno = errno;
    log_message(
        LOG_LEVEL_WARNING,
        "latency observation degraded: could not monitor fping output: %s",
        strerror(saved_errno)
    );
    loop->observation.latency_observation_failed = true;
    close_latency(loop);
    return false;
}

static void handle_traffic_timer(
    struct uloop_interval *timer
)
{
    struct event_loop *loop = (struct event_loop *)(void *)(
        (unsigned char *)(void *)timer -
        offsetof(struct event_loop, traffic_timer)
    );

    observe_traffic_cycle(&loop->observation, loop->config);
    (void)watch_latency(loop);
}

static struct event_loop *event_loop_from_reflector_health_timer(
    struct uloop_interval *timer
)
{
    return (struct event_loop *)(void *)(
        (unsigned char *)(void *)timer -
        offsetof(struct event_loop, reflector_health_timer)
    );
}

static void handle_reflector_health_timer(struct uloop_interval *timer)
{
    struct event_loop *loop = event_loop_from_reflector_health_timer(timer);
    uint64_t timestamp_microseconds;
    size_t index;

    if (!monotonic_microseconds(&timestamp_microseconds)) {
        if (!loop->observation.health_clock_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "reflector health check degraded: monotonic clock failed: %s",
                strerror(errno)
            );
        }
        loop->observation.health_clock_failed = true;
        return;
    }
    if (loop->observation.health_clock_failed) {
        log_message(
            LOG_LEVEL_NOTICE,
            "reflector health check recovered: monotonic clock available"
        );
        loop->observation.health_clock_failed = false;
    }

    for (index = 0U; index < (size_t)loop->config->no_pingers; index++) {
        enum reflector_health_result result = reflector_health_check(
            &loop->observation.reflector_health[index],
            timestamp_microseconds
        );

        if (result == REFLECTOR_HEALTHY) {
            continue;
        }
        log_message(
            LOG_LEVEL_DEBUG,
            "no ping response from reflector: %s within"
            " reflector_response_deadline: %.3fs",
            loop->config->reflectors[index],
            (double)loop->config->reflector_response_deadline_microseconds /
                1000000.0
        );
        log_message(
            LOG_LEVEL_DEBUG,
            "reflector=%s, sum_reflector_offences=%zu and"
            " reflector_misbehaving_detection_thr=%" PRIu64,
            loop->config->reflectors[index],
            loop->observation.reflector_health[index].offence_count,
            loop->config->reflector_misbehaving_detection_threshold
        );
        if (result == REFLECTOR_MISBEHAVING) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Warning: reflector: %s seems to be misbehaving.",
                loop->config->reflectors[index]
            );
        }
    }
}

int monitor_run(const struct sqm_mon_config *config)
{
    /* Match cake-autorate's startup rounding to per-thousand and percent. */
    const struct controller_config controller_config = {
        .download = {
            .adjust = config->adjust_download,
            .minimum_rate_bits_per_second =
                config->minimum_download_rate_bits_per_second,
            .base_rate_bits_per_second =
                config->base_download_rate_bits_per_second,
            .maximum_rate_bits_per_second =
                config->maximum_download_rate_bits_per_second,
            .average_delay_maximum_adjust_up_microseconds =
                config->download_average_owd_delta_maximum_adjust_up_microseconds,
            .delay_threshold_microseconds =
                config->download_owd_delta_delay_threshold_microseconds,
            .average_delay_maximum_adjust_down_microseconds =
                config->download_average_owd_delta_maximum_adjust_down_microseconds
        },
        .upload = {
            .adjust = config->adjust_upload,
            .minimum_rate_bits_per_second =
                config->minimum_upload_rate_bits_per_second,
            .base_rate_bits_per_second =
                config->base_upload_rate_bits_per_second,
            .maximum_rate_bits_per_second =
                config->maximum_upload_rate_bits_per_second,
            .average_delay_maximum_adjust_up_microseconds =
                config->upload_average_owd_delta_maximum_adjust_up_microseconds,
            .delay_threshold_microseconds =
                config->upload_owd_delta_delay_threshold_microseconds,
            .average_delay_maximum_adjust_down_microseconds =
                config->upload_average_owd_delta_maximum_adjust_down_microseconds
        },
        .bufferbloat_detection_window =
            (unsigned int)config->bufferbloat_detection_window,
        .bufferbloat_detection_threshold =
            (unsigned int)config->bufferbloat_detection_threshold,
        .rate_minimum_adjust_down_bufferbloat_per_thousand = rounded_divide(
            config->shaper_rate_minimum_adjust_down_bufferbloat_per_million,
            1000U
        ),
        .rate_maximum_adjust_down_bufferbloat_per_thousand = rounded_divide(
            config->shaper_rate_maximum_adjust_down_bufferbloat_per_million,
            1000U
        ),
        .rate_minimum_adjust_up_high_load_per_thousand = rounded_divide(
            config->shaper_rate_minimum_adjust_up_load_high_per_million,
            1000U
        ),
        .rate_maximum_adjust_up_high_load_per_thousand = rounded_divide(
            config->shaper_rate_maximum_adjust_up_load_high_per_million,
            1000U
        ),
        .rate_adjust_down_low_load_per_thousand = rounded_divide(
            config->shaper_rate_adjust_down_load_low_per_million,
            1000U
        ),
        .rate_adjust_up_low_load_per_thousand = rounded_divide(
            config->shaper_rate_adjust_up_load_low_per_million,
            1000U
        ),
        .high_load_threshold_percent = rounded_divide(
            config->high_load_threshold_per_million,
            10000U
        ),
        .bufferbloat_refractory_period_microseconds =
            config->bufferbloat_refractory_period_microseconds,
        .decay_refractory_period_microseconds =
            config->decay_refractory_period_microseconds
    };
    const struct latency_tracker_config latency_tracker_config = {
        .alpha_baseline_increase_per_million =
            config->alpha_baseline_increase_per_million,
        .alpha_baseline_decrease_per_million =
            config->alpha_baseline_decrease_per_million,
        .alpha_delta_ewma_per_million =
            config->alpha_delta_ewma_per_million
    };
    const struct reflector_health_config reflector_health_config = {
        .response_deadline_microseconds =
            config->reflector_response_deadline_microseconds,
        .detection_window =
            (size_t)config->reflector_misbehaving_detection_window,
        .detection_threshold =
            (size_t)config->reflector_misbehaving_detection_threshold
    };
    struct event_loop loop = {
        .observation = {
            .download = {
                .name = "download",
                .interface = config->ingress_interface,
                .cake_state = CAKE_OBSERVATION_UNKNOWN,
                .traffic_state = TRAFFIC_OBSERVATION_UNKNOWN
            },
            .upload = {
                .name = "upload",
                .interface = config->interface,
                .cake_state = CAKE_OBSERVATION_UNKNOWN,
                .traffic_state = TRAFFIC_OBSERVATION_UNKNOWN
            },
            .latency_observation_failed = false,
            .traffic_clock_failed = false
        },
        .config = config,
        .traffic_timer = {
            .cb = handle_traffic_timer
        },
        .reflector_health_timer = {
            .cb = handle_reflector_health_timer
        },
        .latency_output = {
            .cb = handle_latency_output,
            .fd = -1
        },
        .result = -1
    };
    bool previous_sigchld_handling = uloop_handle_sigchld;
    int run_status;
    uint64_t start_microseconds;
    size_t health_count = 0U;
    size_t index;

    latency_init(&loop.observation.latency);
    netlink_init(&loop.observation.netlink);
    traffic_monitor_init(&loop.observation.download.traffic_monitor);
    traffic_monitor_init(&loop.observation.upload.traffic_monitor);
    if (controller_init(
            &loop.observation.controller,
            &controller_config
        ) != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not initialize controller: %s",
            strerror(errno)
        );
        goto done;
    }
    for (index = 0U; index < (size_t)config->no_pingers; index++) {
        if (latency_tracker_init(
                &loop.observation.latency_trackers[index],
                &latency_tracker_config
            ) != 0) {
            log_message(
                LOG_LEVEL_ERROR,
                "could not initialize latency tracker: %s",
                strerror(errno)
            );
            goto done;
        }
    }
    if (!monotonic_microseconds(&start_microseconds)) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not initialize reflector health clock: %s",
            strerror(errno)
        );
        goto done;
    }
    for (index = 0U; index < (size_t)config->no_pingers; index++) {
        if (reflector_health_init(
                &loop.observation.reflector_health[index],
                &reflector_health_config,
                start_microseconds
            ) != 0) {
            log_message(
                LOG_LEVEL_ERROR,
                "could not initialize reflector health: %s",
                strerror(errno)
            );
            goto done;
        }
        health_count++;
    }

    /* latency.c owns and reaps fping; uloop must not consume its SIGCHLD. */
    uloop_handle_sigchld = false;
    if (uloop_init() != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not initialize event loop: %s",
            strerror(errno)
        );
        uloop_handle_sigchld = previous_sigchld_handling;
        goto done;
    }

    if (uloop_interval_set(
            &loop.traffic_timer,
            (unsigned int)(
                config->monitor_achieved_rates_interval_microseconds / 1000U
            )
        ) != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not monitor traffic timer: %s",
            strerror(errno)
        );
        goto uloop_done;
    }
    if (uloop_interval_set(
            &loop.reflector_health_timer,
            (unsigned int)(
                config->reflector_health_check_interval_microseconds / 1000U
            )
        ) != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not monitor reflector health timer: %s",
            strerror(errno)
        );
        goto uloop_done;
    }

    observe_traffic_cycle(&loop.observation, config);
    (void)watch_latency(&loop);
    run_status = uloop_run();
    if (run_status == SIGINT || run_status == SIGTERM) {
        log_message(
            LOG_LEVEL_NOTICE,
            "received signal %d; shutting down",
            run_status
        );
        loop.result = 0;
    }

uloop_done:
    close_latency(&loop);
    (void)uloop_interval_cancel(&loop.traffic_timer);
    (void)uloop_interval_cancel(&loop.reflector_health_timer);
    uloop_done();
    uloop_handle_sigchld = previous_sigchld_handling;

done:
    for (index = 0U; index < health_count; index++) {
        reflector_health_cleanup(&loop.observation.reflector_health[index]);
    }
    netlink_close(&loop.observation.netlink);
    latency_close(&loop.observation.latency);
    controller_close(&loop.observation.controller);
    return loop.result;
}
