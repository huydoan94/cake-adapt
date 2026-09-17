#define _GNU_SOURCE

#include "monitor.h"

#include "cake.h"
#include "controller.h"
#include "cpu.h"
#include "helpers.h"
#include "latency.h"
#include "log.h"
#include "netlink.h"
#include "traffic.h"

#include <libubox/list.h>
#include <libubox/uloop.h>
#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <linux/pkt_sched.h>
#include <net/if.h>
#include <signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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
    struct traffic_monitor traffic_monitor;
    struct cake_observation cake;
    enum cake_observation_state cake_state;
    enum traffic_observation_state traffic_state;
    uint64_t traffic_rate_bits_per_second;
    bool cake_valid;
    bool traffic_valid;
    uint64_t next_cake_observation_microseconds;
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
        traffic_init(&direction->traffic_monitor);
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

    update_result = traffic_update(
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
    struct netlink *netlink,
    struct monitored_direction *direction,
    uint64_t timestamp_microseconds,
    uint64_t retry_interval_microseconds
)
{
    char error[ERROR_SIZE] = "";
    enum cake_read_result read_result;

    if (
        timestamp_microseconds <
        direction->next_cake_observation_microseconds
    ) {
        direction->cake_valid = false;
        return;
    }
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
        direction->next_cake_observation_microseconds = 0U;
        log_cake_sample(direction->interface, &direction->cake);
        return;
    case CAKE_READ_NOT_FOUND:
        if (direction->cake_state != CAKE_OBSERVATION_NOT_FOUND) {
            log_message(
                LOG_LEVEL_WARNING,
                direction->cake_state == CAKE_OBSERVATION_AVAILABLE
                    ? "CAKE observation degraded: no CAKE qdisc found on interface=%s"
                    : "CAKE not found: interface=%s; observation will retry",
                direction->interface
            );
        }
        direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
        /* RTM_NEWQDISC wakes discovery when CAKE is created. */
        direction->next_cake_observation_microseconds = UINT64_MAX;
        return;
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
    direction->next_cake_observation_microseconds =
        timestamp_microseconds + retry_interval_microseconds;
}

struct observation_context {
    struct controller controller;
    struct latency latency;
    struct latency_tracker latency_trackers[CONFIG_MAX_REFLECTORS];
    struct reflector_health reflector_health[CONFIG_MAX_REFLECTORS];
    size_t reflector_order[CONFIG_MAX_REFLECTORS];
    struct netlink netlink;
    struct monitored_direction download;
    struct monitored_direction upload;
    bool latency_observation_failed;
    bool traffic_clock_failed;
    bool health_clock_failed;
    uint64_t last_reflector_replacement_microseconds;
    uint64_t last_reflector_comparison_microseconds;
    uint64_t last_reflector_response_microseconds;
    uint64_t last_pinger_restart_microseconds;
    uint64_t next_latency_attempt_microseconds;
    uint64_t pinger_grace_until_microseconds;
    struct controller_activity activity;
};

struct event_loop {
    struct observation_context observation;
    const struct config *config;
    struct uloop_interval traffic_timer;
    struct uloop_interval reflector_health_timer;
    struct uloop_interval cpu_timer;
    struct uloop_interval log_timer;
    struct uloop_signal log_export_signal;
    struct uloop_signal log_reset_signal;
    struct cpu_monitor cpu_monitor;
    size_t cpu_count;
    bool cpu_observation_failed;
    bool qdisc_refresh;
    struct uloop_fd qdisc_events;
    struct uloop_fd latency_output;
    int result;
};

static void handle_traffic_timer(struct uloop_interval *timer);

static bool cake_ready(const struct observation_context *context)
{
    return context->download.cake_state == CAKE_OBSERVATION_AVAILABLE &&
        context->upload.cake_state == CAKE_OBSERVATION_AVAILABLE;
}

static bool random_index(size_t count, size_t *index)
{
    uint32_t value;
    /* no_pingers is validated as 1..CONFIG_MAX_REFLECTORS at startup. */
    if (getentropy(&value, sizeof(value)) != 0) {
        return false;
    }
    *index = (size_t)(value % (uint32_t)count);
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

static bool direction_has_low_load(
    const struct monitored_direction *direction,
    uint64_t high_load_threshold_percent
)
{
    return direction->traffic_valid &&
        load_percent(
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

    if (load_percent(traffic_rate, cake_rate) >
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
    const struct config *config,
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
            .download_load_percent = load_percent(
                input->download.traffic_rate_bits_per_second,
                input->download.cake_rate_bits_per_second
            ),
            .upload_load_percent = load_percent(
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
    struct netlink *netlink,
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
    struct controller *controller,
    struct netlink *netlink,
    const struct config *config,
    struct monitored_direction *download,
    struct monitored_direction *upload,
    const struct latency_observation *latency,
    bool latency_valid,
    const char *reflector
)
{
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
    const struct {
        struct monitored_direction *direction;
        const struct controller_direction_input *input;
        const struct controller_direction_output *output;
    } directions[] = {
        { download, &input.download, &output.download },
        { upload, &input.upload, &output.upload }
    };

    (void)read_clock_microseconds(CLOCK_MONOTONIC, &input.timestamp_microseconds);

    controller_update(controller, &input, &output);
    for (size_t index = 0U; index < ARRAY_SIZE(directions); index++) {
        struct monitored_direction *direction = directions[index].direction;
        const struct controller_direction_output *decision = directions[index].output;

        if (decision->state_changed) {
            log_line_state(direction->name, decision->state, directions[index].input);
        }
        if (decision->congestion_changed) {
            log_congestion_state(direction->name, decision->congestion, &input.latency);
        }
        /* The controller never requests changes for an observation-only link. */
        if (decision->rate_changed) {
            apply_bandwidth(
                netlink,
                direction,
                decision->rate_bits_per_second,
                decision->rate_reason,
                config->output_cake_changes
            );
        }
    }
    if (latency_valid) {
        log_controller_stats(config, &input, &output, latency, reflector);
    }
}

static void observe_traffic_cycle(
    struct observation_context *context,
    const struct config *config
)
{
    struct timespec traffic_timestamp;
    uint64_t timestamp_microseconds;

    if (clock_gettime(CLOCK_MONOTONIC, &traffic_timestamp) != 0) {
        if (!context->traffic_clock_failed) {
            log_message(
                LOG_LEVEL_WARNING,
                "traffic observation degraded: monotonic clock failed: %s",
                strerror(errno)
            );
        }
        context->traffic_clock_failed = true;
        context->download.cake_valid = false;
        context->upload.cake_valid = false;
        context->download.traffic_valid = false;
        context->upload.traffic_valid = false;
        traffic_init(&context->download.traffic_monitor);
        traffic_init(&context->upload.traffic_monitor);
    } else {
        timestamp_microseconds =
            (uint64_t)traffic_timestamp.tv_sec * 1000000U +
            (uint64_t)traffic_timestamp.tv_nsec / 1000U;
        observe_cake(
            &context->netlink,
            &context->upload,
            timestamp_microseconds,
            config->interface_up_check_interval_microseconds
        );
        observe_cake(
            &context->netlink,
            &context->download,
            timestamp_microseconds,
            config->interface_up_check_interval_microseconds
        );
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
    const struct config *config
)
{
    const char *targets[CONFIG_MAX_REFLECTORS];
    char error[ERROR_SIZE] = "";
    size_t target_count = (size_t)config->no_pingers;
    size_t index;
    uint64_t timestamp_microseconds;

    if (!cake_ready(context)) {
        return false;
    }
    if (latency_is_open(&context->latency)) {
        return true;
    }
    if (
        !read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds) ||
        timestamp_microseconds < context->next_latency_attempt_microseconds
    ) {
        return false;
    }
    context->next_latency_attempt_microseconds = timestamp_microseconds +
        config->interface_up_check_interval_microseconds;
    for (index = 0U; index < target_count; index++) {
        targets[index] = config->reflectors[context->reflector_order[index]];
    }
    if (latency_open(
            &context->latency,
            config->interface,
            targets,
            target_count,
            config->reflector_ping_interval_microseconds,
            config->ping_extra_args,
            config->ping_prefix_string,
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
    context->next_latency_attempt_microseconds = 0U;
    context->last_pinger_restart_microseconds = timestamp_microseconds;
    return true;
}

static size_t find_active_reflector(
    const struct observation_context *context,
    const struct config *config,
    const char *target
)
{
    size_t target_count = (size_t)config->no_pingers;
    size_t index;

    for (index = 0U; index < target_count; index++) {
        if (strcmp(
                config->reflectors[context->reflector_order[index]],
                target
            ) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool receive_latency_samples(
    struct observation_context *context,
    const struct config *config
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

        reflector_index = find_active_reflector(context, config, sample.target);
        if (reflector_index == SIZE_MAX) {
            log_message(
                LOG_LEVEL_WARNING,
                "latency observation degraded: unexpected reflector=%s",
                sample.target
            );
            context->latency_observation_failed = true;
            return false;
        }

        tracker_update(
            &context->latency_trackers[
                context->reflector_order[reflector_index]
            ],
            &sample,
            &observation
        );
        {
            uint64_t response_timestamp_microseconds;

            if (read_clock_microseconds(CLOCK_MONOTONIC, &response_timestamp_microseconds)) {
                context->last_reflector_response_microseconds =
                    response_timestamp_microseconds;
                health_record_response(
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

        tracker_update_delta_ewma(
            &context->latency_trackers[
                context->reflector_order[reflector_index]
            ],
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

static void close_latency(struct event_loop *loop)
{
    if (loop->latency_output.registered) {
        (void)uloop_fd_delete(&loop->latency_output);
    }
    loop->latency_output.fd = -1;
    latency_close(&loop->observation.latency);
}

static struct monitored_direction *event_direction(
    struct event_loop *loop,
    const struct qdisc_event *event
)
{
    struct monitored_direction *directions[] = {
        &loop->observation.download,
        &loop->observation.upload
    };
    size_t index;

    if (event->parent != TC_H_ROOT) {
        return NULL;
    }
    for (index = 0U; index < ARRAY_SIZE(directions); index++) {
        if (
            if_nametoindex(directions[index]->interface) ==
            event->interface_index
        ) {
            return directions[index];
        }
    }
    return NULL;
}

static void reset_traffic_observation(struct monitored_direction *direction)
{
    direction->traffic_valid = false;
    direction->traffic_state = TRAFFIC_OBSERVATION_UNKNOWN;
    direction->traffic_rate_bits_per_second = 0U;
    traffic_init(&direction->traffic_monitor);
}

static int process_qdisc_event(
    const struct qdisc_event *event,
    void *context
)
{
    struct event_loop *loop = context;
    struct monitored_direction *direction = event_direction(loop, event);

    if (direction == NULL) {
        return 0;
    }
    if (event->type == QDISC_REMOVED) {
        if (
            direction->cake_state != CAKE_OBSERVATION_AVAILABLE ||
            direction->cake.handle != event->handle
        ) {
            return 0;
        }
        log_message(
            LOG_LEVEL_NOTICE,
            "CAKE removed: direction=%s interface=%s handle=0x%08" PRIx32
            "; monitoring suspended",
            direction->name,
            direction->interface,
            event->handle
        );
        memset(&direction->cake, 0, sizeof(direction->cake));
        direction->cake_valid = false;
        direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
        direction->next_cake_observation_microseconds = UINT64_MAX;
        reset_traffic_observation(direction);
        close_latency(loop);
        return 0;
    }

    /* Bandwidth changes notify RTM_NEWQDISC with the existing handle. */
    if (
        direction->cake_state == CAKE_OBSERVATION_AVAILABLE &&
        direction->cake.handle == event->handle
    ) {
        return 0;
    }
    if (direction->cake_state == CAKE_OBSERVATION_AVAILABLE) {
        direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
    }
    direction->cake_valid = false;
    direction->next_cake_observation_microseconds = 0U;
    reset_traffic_observation(direction);
    loop->qdisc_refresh = true;
    return 0;
}

static void handle_qdisc_events(
    struct uloop_fd *descriptor,
    unsigned int events
)
{
    struct event_loop *loop = __extension__ container_of(
        descriptor,
        struct event_loop,
        qdisc_events
    );
    char error[ERROR_SIZE] = "";

    (void)events;
    if (
        netlink_receive_qdisc_events(
            &loop->observation.netlink,
            error,
            sizeof(error)
        ) != 0
    ) {
        log_message(LOG_LEVEL_ERROR, "qdisc lifecycle monitoring failed: %s", error);
        loop->result = -1;
        uloop_end();
        return;
    }
    if (loop->qdisc_refresh) {
        loop->qdisc_refresh = false;
        handle_traffic_timer(&loop->traffic_timer);
    }
}

static void handle_latency_output(
    struct uloop_fd *descriptor,
    unsigned int events
)
{
    /* libubox's container_of uses a GNU expression; keep the exception local. */
    struct event_loop *loop = __extension__ container_of(
        descriptor,
        struct event_loop,
        latency_output
    );

    (void)events;
    if (!receive_latency_samples(&loop->observation, loop->config)) {
        uint64_t timestamp_microseconds;

        close_latency(loop);
        if (read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
            loop->observation.next_latency_attempt_microseconds =
                timestamp_microseconds +
                loop->config->interface_up_check_interval_microseconds;
        }
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

static void enforce_minimum_rates(
    struct observation_context *context,
    const struct config *config,
    uint64_t timestamp_microseconds
)
{
    controller_set_minimum_rates(
        &context->controller,
        timestamp_microseconds
    );
    if (config->adjust_download && context->download.cake_valid) {
        apply_bandwidth(
            &context->netlink,
            &context->download,
            config->minimum_download_rate_bits_per_second,
            CONTROLLER_RATE_RECONCILE,
            config->output_cake_changes
        );
    }
    if (config->adjust_upload && context->upload.cake_valid) {
        apply_bandwidth(
            &context->netlink,
            &context->upload,
            config->minimum_upload_rate_bits_per_second,
            CONTROLLER_RATE_RECONCILE,
            config->output_cake_changes
        );
    }
}

static void reset_reflector_health(
    struct observation_context *context,
    const struct config *config,
    uint64_t timestamp_microseconds
)
{
    size_t index;

    for (index = 0U; index < (size_t)config->no_pingers; index++) {
        health_reset(
            &context->reflector_health[index],
            timestamp_microseconds
        );
    }
}

static void restart_latency(
    struct event_loop *loop,
    uint64_t timestamp_microseconds
)
{
    close_latency(loop);
    loop->observation.next_latency_attempt_microseconds = 0U;
    reset_reflector_health(
        &loop->observation,
        loop->config,
        timestamp_microseconds
    );
    loop->observation.last_pinger_restart_microseconds =
        timestamp_microseconds;
    (void)watch_latency(loop);
}

static void update_monitor_state(
    struct event_loop *loop,
    uint64_t timestamp_microseconds
)
{
    static const char *const names[] = { "RUNNING", "IDLE", "STALL" };
    struct observation_context *context = &loop->observation;
    const struct config *config = loop->config;
    const struct controller_activity_config activity_config = {
        .enable_sleep = config->enable_sleep_function,
        .active_threshold_bits_per_second =
            config->connection_active_threshold_bits_per_second,
        .stall_threshold_bits_per_second =
            config->connection_stall_threshold_bits_per_second,
        .sustained_idle_microseconds =
            config->sustained_idle_sleep_threshold_microseconds,
        .stall_timeout_microseconds = config->stall_detection_threshold *
            (config->reflector_ping_interval_microseconds / config->no_pingers),
        .global_timeout_microseconds =
            config->global_ping_response_timeout_microseconds
    };
    const struct controller_activity_input input = {
        .download = {
            .valid = context->download.traffic_valid,
            .traffic_rate_bits_per_second =
                context->download.traffic_rate_bits_per_second
        },
        .upload = {
            .valid = context->upload.traffic_valid,
            .traffic_rate_bits_per_second =
                context->upload.traffic_rate_bits_per_second
        },
        .timestamp_microseconds = timestamp_microseconds,
        .last_response_microseconds =
            context->last_reflector_response_microseconds,
        .last_pinger_start_microseconds =
            context->last_pinger_restart_microseconds,
        .grace_until_microseconds = context->pinger_grace_until_microseconds
    };
    enum controller_activity_state previous = context->activity.state;
    struct controller_activity_output output;

    activity_update(
        &context->activity,
        &activity_config,
        &input,
        &output
    );
    if (output.check_stall_loads) {
        log_message(
            LOG_LEVEL_DEBUG,
            "Warning: no reflector response within: %.2f seconds. Checking loads.",
            (double)activity_config.stall_timeout_microseconds / 1000000.0
        );
        log_message(
            LOG_LEVEL_DEBUG,
            "load check is: (( %" PRIu64 " kbps > %" PRIu64
            " kbps for download && %" PRIu64 " kbps > %" PRIu64
            " kbps for upload ))",
            context->download.traffic_rate_bits_per_second / 1000U,
            config->connection_stall_threshold_bits_per_second / 1000U,
            context->upload.traffic_rate_bits_per_second / 1000U,
            config->connection_stall_threshold_bits_per_second / 1000U
        );
        if (context->activity.state == CONTROLLER_RUNNING) {
            log_message(
                LOG_LEVEL_DEBUG,
                "load above connection stall threshold so resuming normal operation."
            );
        }
    }
    if (output.global_timeout_started) {
        if (config->minimum_shaper_rates_enforcement) {
            enforce_minimum_rates(
                context,
                config,
                timestamp_microseconds
            );
        }
        log_system_message(
            "Warning: Configured global ping response timeout: %.3f seconds exceeded.",
            (double)activity_config.global_timeout_microseconds / 1000000.0
        );
    }
    if (output.state_changed) {
        if (context->activity.state == CONTROLLER_RUNNING) {
            log_message(
                LOG_LEVEL_DEBUG,
                previous == CONTROLLER_IDLE
                    ? "Connection load exceeded active threshold. Resuming normal operation."
                    : "Connection stall ended. Resuming normal operation."
            );
        }
        log_message(
            LOG_LEVEL_DEBUG,
            "Changing main state from: %s to: %s",
            names[previous],
            names[context->activity.state]
        );
        if (context->activity.state == CONTROLLER_IDLE) {
            log_message(LOG_LEVEL_DEBUG, "Connection idle. Waiting for minimum load.");
            if (config->minimum_shaper_rates_enforcement) {
                log_message(LOG_LEVEL_DEBUG, "Enforcing minimum shaper rates.");
                enforce_minimum_rates(
                    context,
                    config,
                    timestamp_microseconds
                );
            }
            close_latency(loop);
        } else if (previous == CONTROLLER_IDLE) {
            context->last_reflector_response_microseconds = timestamp_microseconds;
            /* Match cake-autorate's two-period setup grace after waking. */
            context->pinger_grace_until_microseconds = timestamp_microseconds +
                2U * config->reflector_ping_interval_microseconds;
            reset_reflector_health(
                context,
                config,
                context->pinger_grace_until_microseconds
            );
            context->next_latency_attempt_microseconds = 0U;
            (void)watch_latency(loop);
        }
    }
    if (output.restart_pingers) {
        log_message(LOG_LEVEL_DEBUG, "Restarting pingers.");
        restart_latency(loop, timestamp_microseconds);
    }
}

static bool replace_active_reflector(
    struct event_loop *loop,
    size_t pinger,
    uint64_t timestamp_microseconds
)
{
    struct observation_context *context = &loop->observation;
    const struct config *config = loop->config;
    size_t active_count = (size_t)config->no_pingers;
    size_t reflector_count = (size_t)config->reflector_count;
    size_t bad_index = context->reflector_order[pinger];

    if (reflector_count <= active_count) {
        log_message(
            LOG_LEVEL_DEBUG,
            "No additional reflectors specified so just retaining: %s.",
            config->reflectors[bad_index]
        );
        health_reset(
            &context->reflector_health[pinger],
            timestamp_microseconds
        );
        log_message(
            LOG_LEVEL_DEBUG,
            "Resetting reflector offences associated with reflector: %s.",
            config->reflectors[bad_index]
        );
        return true;
    }

    log_message(
        LOG_LEVEL_DEBUG,
        "replacing reflector: %s with %s.",
        config->reflectors[bad_index],
        config->reflectors[context->reflector_order[active_count]]
    );
    if (config->retain_reflector_stats) {
        log_message(
            LOG_LEVEL_DEBUG,
            "Retaining reflector stats associated with: %s",
            config->reflectors[bad_index]
        );
    } else {
        log_message(
            LOG_LEVEL_DEBUG,
            "Discarding reflector stats associated with %s",
            config->reflectors[bad_index]
        );
        tracker_reset(&context->latency_trackers[bad_index]);
    }

    reflector_rotate(
        context->reflector_order,
        reflector_count,
        active_count,
        pinger
    );
    health_reset(
        &context->reflector_health[pinger],
        timestamp_microseconds
    );
    log_message(
        LOG_LEVEL_DEBUG,
        "Resetting reflector offences associated with reflector: %s.",
        config->reflectors[context->reflector_order[pinger]]
    );

    /* fping owns all active targets in one process, so rotate them together. */
    close_latency(loop);
    context->next_latency_attempt_microseconds = 0U;
    (void)watch_latency(loop);
    return true;
}

static void handle_traffic_timer(
    struct uloop_interval *timer
)
{
    struct event_loop *loop = __extension__ container_of(
        timer,
        struct event_loop,
        traffic_timer
    );

    uint64_t timestamp_microseconds;

    observe_traffic_cycle(&loop->observation, loop->config);
    if (!cake_ready(&loop->observation)) {
        close_latency(loop);
        return;
    }
    if (read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
        update_monitor_state(loop, timestamp_microseconds);
    }
    if (loop->observation.activity.state != CONTROLLER_IDLE) {
        (void)watch_latency(loop);
    }
}

static void observe_cpu(
    struct event_loop *loop,
    bool emit_records
)
{
    struct cpu_sample sample = { 0 };
    unsigned int usage[CPU_MAX_COUNT];
    char error[ERROR_SIZE] = "";

    if (cpu_read("/proc/stat", &sample, error, sizeof(error)) != 0) {
        if (!loop->cpu_observation_failed) {
            log_message(LOG_LEVEL_WARNING, "CPU observation degraded: %s", error);
        }
        loop->cpu_observation_failed = true;
        return;
    }
    if (loop->cpu_observation_failed) {
        log_message(LOG_LEVEL_NOTICE, "CPU observation recovered");
        loop->cpu_observation_failed = false;
    }
    if (loop->cpu_count != sample.count) {
        loop->cpu_count = sample.count;
        cpu_init(&loop->cpu_monitor);
        log_message(LOG_LEVEL_DEBUG, "Detected %zu CPU cores.", sample.count - 1U);
        log_print_cpu_headers(
            &sample,
            loop->config->output_cpu_stats,
            loop->config->output_cpu_raw_stats
        );
    }
    if (!emit_records) {
        return;
    }
    if (loop->config->output_cpu_raw_stats) {
        log_cpu_raw(&sample);
    }
    if (loop->config->output_cpu_stats) {
        cpu_usage(&loop->cpu_monitor, &sample, usage);
        log_cpu(&sample, usage);
    }
}

static void handle_cpu_timer(struct uloop_interval *timer)
{
    struct event_loop *loop = __extension__ container_of(
        timer,
        struct event_loop,
        cpu_timer
    );

    if (loop->observation.activity.state == CONTROLLER_RUNNING) {
        observe_cpu(loop, true);
    }
}

static void handle_log_timer(struct uloop_interval *timer)
{
    (void)timer;
    log_tick();
}

static void handle_log_export_signal(struct uloop_signal *signal)
{
    char export_path[CONFIG_STRING_SIZE + 32U];

    (void)signal;
    log_message(
        LOG_LEVEL_DEBUG,
        "received log file export signal so exporting log file."
    );
    if (log_export_file(export_path, sizeof(export_path)) != 0) {
        log_message(LOG_LEVEL_WARNING, "log file export failed: %s", strerror(errno));
    }
}

static void handle_log_reset_signal(struct uloop_signal *signal)
{
    (void)signal;
    log_message(
        LOG_LEVEL_DEBUG,
        "received log file reset signal so flushing log and resetting log file."
    );
    if (log_reset_file() != 0) {
        log_message(LOG_LEVEL_WARNING, "log file reset failed: %s", strerror(errno));
    }
}

static bool signed_delta_exceeds(int64_t delta, uint64_t threshold)
{
    return delta > 0 && (uint64_t)delta > threshold;
}

static bool compare_active_reflectors(
    struct event_loop *loop,
    uint64_t timestamp_microseconds
)
{
    struct reflector_comparison comparisons[CONFIG_MAX_REFLECTORS];
    size_t active_count = (size_t)loop->config->no_pingers;
    size_t index;

    reflector_compare(
        loop->observation.latency_trackers,
        loop->observation.reflector_order,
        active_count,
        comparisons
    );

    for (index = 0U; index < active_count; index++) {
        const struct reflector_comparison *comparison = &comparisons[index];
        const char *reflector = loop->config->reflectors[
            loop->observation.reflector_order[index]
        ];

        if (loop->config->output_reflector_stats) {
            const struct log_reflector_record record = {
                .reflector = reflector,
                .minimum_sum_owd_baselines_microseconds =
                    comparison->minimum_sum_owd_baselines_microseconds,
                .sum_owd_baselines_microseconds =
                    comparison->sum_owd_baselines_microseconds,
                .sum_owd_baselines_delta_microseconds =
                    comparison->sum_owd_baselines_delta_microseconds,
                .sum_owd_baselines_delta_threshold_microseconds =
                    loop->config
                        ->reflector_sum_owd_baselines_delta_threshold_microseconds,
                .minimum_download_delta_ewma_microseconds =
                    comparison->minimum_download_delta_ewma_microseconds,
                .download_delta_ewma_microseconds =
                    comparison->download_delta_ewma_microseconds,
                .download_delta_ewma_delta_microseconds =
                    comparison->download_delta_ewma_delta_microseconds,
                .delta_ewma_delta_threshold_microseconds =
                    loop->config
                        ->reflector_owd_delta_ewma_delta_threshold_microseconds,
                .minimum_upload_delta_ewma_microseconds =
                    comparison->minimum_upload_delta_ewma_microseconds,
                .upload_delta_ewma_microseconds =
                    comparison->upload_delta_ewma_microseconds,
                .upload_delta_ewma_delta_microseconds =
                    comparison->upload_delta_ewma_delta_microseconds
            };

            log_reflector(&record);
        }

        if (comparison->sum_owd_baselines_delta_microseconds >
            loop->config
                ->reflector_sum_owd_baselines_delta_threshold_microseconds) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Warning: reflector: %s sum_owd_baselines_us exceeds the"
                " minimum by set threshold.",
                reflector
            );
        } else if (signed_delta_exceeds(
                comparison->download_delta_ewma_delta_microseconds,
                loop->config
                    ->reflector_owd_delta_ewma_delta_threshold_microseconds
            )) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Warning: reflector: %s dl_owd_delta_ewma_us exceeds the"
                " minimum by set threshold.",
                reflector
            );
        } else if (signed_delta_exceeds(
                comparison->upload_delta_ewma_delta_microseconds,
                loop->config
                    ->reflector_owd_delta_ewma_delta_threshold_microseconds
            )) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Warning: reflector: %s ul_owd_delta_ewma_us exceeds the"
                " minimum by set threshold.",
                reflector
            );
        } else {
            continue;
        }

        (void)replace_active_reflector(loop, index, timestamp_microseconds);
        return true;
    }
    return false;
}

static bool run_scheduled_reflector_work(
    struct event_loop *loop,
    uint64_t timestamp_microseconds
)
{
    uint64_t replacement_interval_microseconds =
        loop->config->reflector_replacement_interval_minutes * 60000000U;
    uint64_t comparison_interval_microseconds =
        loop->config->reflector_comparison_interval_minutes * 60000000U;
    size_t pinger;

    if (interval_elapsed(
            timestamp_microseconds,
            loop->observation.last_reflector_replacement_microseconds,
            replacement_interval_microseconds
        )) {
        loop->observation.last_reflector_replacement_microseconds =
            timestamp_microseconds;
        if (!random_index((size_t)loop->config->no_pingers, &pinger)) {
            log_message(
                LOG_LEVEL_WARNING,
                "could not randomly select reflector for replacement: %s",
                strerror(errno)
            );
            return false;
        }
        log_message(
            LOG_LEVEL_DEBUG,
            "reflector: %s randomly selected for replacement.",
            loop->config->reflectors[
                loop->observation.reflector_order[pinger]
            ]
        );
        (void)replace_active_reflector(
            loop,
            pinger,
            timestamp_microseconds
        );
        return true;
    }

    if (interval_elapsed(
            timestamp_microseconds,
            loop->observation.last_reflector_comparison_microseconds,
            comparison_interval_microseconds
        )) {
        loop->observation.last_reflector_comparison_microseconds =
            timestamp_microseconds;
        return compare_active_reflectors(loop, timestamp_microseconds);
    }
    return false;
}

static void handle_reflector_health_timer(struct uloop_interval *timer)
{
    struct event_loop *loop = __extension__ container_of(
        timer,
        struct event_loop,
        reflector_health_timer
    );
    uint64_t timestamp_microseconds;
    bool reflector_replaced = false;
    size_t index;

    if (
        !cake_ready(&loop->observation) ||
        loop->observation.activity.state != CONTROLLER_RUNNING
    ) {
        return;
    }

    if (!read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
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
    if (timestamp_microseconds < loop->observation.pinger_grace_until_microseconds) {
        return;
    }
    if (run_scheduled_reflector_work(loop, timestamp_microseconds)) {
        return;
    }

    for (index = 0U; index < (size_t)loop->config->no_pingers; index++) {
        enum reflector_health_result result = health_check(
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
            loop->config->reflectors[
                loop->observation.reflector_order[index]
            ],
            (double)loop->config->reflector_response_deadline_microseconds /
                1000000.0
        );
        log_message(
            LOG_LEVEL_DEBUG,
            "reflector=%s, sum_reflector_offences=%zu and"
            " reflector_misbehaving_detection_thr=%" PRIu64,
            loop->config->reflectors[
                loop->observation.reflector_order[index]
            ],
            loop->observation.reflector_health[index].offence_count,
            loop->config->reflector_misbehaving_detection_threshold
        );
        if (result == REFLECTOR_MISBEHAVING) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Warning: reflector: %s seems to be misbehaving.",
                loop->config->reflectors[
                    loop->observation.reflector_order[index]
                ]
            );
            if (!reflector_replaced) {
                reflector_replaced = replace_active_reflector(
                    loop,
                    index,
                    timestamp_microseconds
                );
            } else {
                log_message(
                    LOG_LEVEL_DEBUG,
                    "Warning: skipping replacement of reflector: %s given"
                    " prior replacement within this reflector health check"
                    " cycle.",
                    loop->config->reflectors[
                        loop->observation.reflector_order[index]
                    ]
                );
            }
        }
    }
}

int monitor_run(const struct config *config)
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
        .cpu_timer = {
            .cb = handle_cpu_timer
        },
        .log_timer = {
            .cb = handle_log_timer
        },
        .log_export_signal = {
            .cb = handle_log_export_signal,
            .signo = SIGUSR1
        },
        .log_reset_signal = {
            .cb = handle_log_reset_signal,
            .signo = SIGUSR2
        },
        .qdisc_events = {
            .cb = handle_qdisc_events,
            .fd = -1
        },
        .latency_output = {
            .cb = handle_latency_output,
            .fd = -1
        },
        .result = -1
    };
    bool previous_sigchld_handling = uloop_handle_sigchld;
    const struct {
        struct uloop_interval *timer;
        uint64_t interval_microseconds;
        const char *name;
    } required_timers[] = {
        { &loop.traffic_timer, config->monitor_achieved_rates_interval_microseconds, "traffic" },
        { &loop.reflector_health_timer, config->reflector_health_check_interval_microseconds, "reflector health" }
    };
    int run_status;
    uint64_t start_microseconds;
    size_t health_count = 0U;
    size_t index;

    latency_init(&loop.observation.latency);
    netlink_init(&loop.observation.netlink);
    traffic_init(&loop.observation.download.traffic_monitor);
    traffic_init(&loop.observation.upload.traffic_monitor);
    cpu_init(&loop.cpu_monitor);
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
    for (index = 0U; index < (size_t)config->reflector_count; index++) {
        if (tracker_init(
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
        loop.observation.reflector_order[index] = index;
    }
    if (!read_clock_microseconds(CLOCK_MONOTONIC, &start_microseconds)) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not initialize reflector health clock: %s",
            strerror(errno)
        );
        goto done;
    }
    loop.observation.last_reflector_replacement_microseconds =
        start_microseconds;
    loop.observation.last_reflector_comparison_microseconds =
        start_microseconds;
    loop.observation.last_reflector_response_microseconds =
        start_microseconds;
    loop.observation.last_pinger_restart_microseconds = start_microseconds;
    loop.observation.activity.state = CONTROLLER_RUNNING;
    for (index = 0U; index < (size_t)config->no_pingers; index++) {
        if (health_init(
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

    {
        char error[ERROR_SIZE] = "";

        if (
            netlink_subscribe_qdiscs(
                &loop.observation.netlink,
                process_qdisc_event,
                &loop,
                error,
                sizeof(error)
            ) != 0
        ) {
            log_message(LOG_LEVEL_ERROR, "%s", error);
            goto uloop_done;
        }
        loop.qdisc_events.fd = netlink_event_descriptor(
            &loop.observation.netlink
        );
        if (
            loop.qdisc_events.fd < 0 ||
            uloop_fd_add(
                &loop.qdisc_events,
                ULOOP_READ | ULOOP_ERROR_CB
            ) != 0
        ) {
            log_message(
                LOG_LEVEL_ERROR,
                "could not monitor qdisc lifecycle: %s",
                strerror(errno)
            );
            goto uloop_done;
        }
    }

    for (index = 0; index < ARRAY_SIZE(required_timers); ++index) {
        if (uloop_interval_set(
                required_timers[index].timer,
                (unsigned int)(required_timers[index].interval_microseconds / 1000U)
            ) != 0) {
            log_message(
                LOG_LEVEL_ERROR,
                "could not monitor %s timer: %s",
                required_timers[index].name,
                strerror(errno)
            );
            goto uloop_done;
        }
    }

    observe_traffic_cycle(&loop.observation, config);
    if (config->output_cpu_stats || config->output_cpu_raw_stats) {
        observe_cpu(&loop, false);
        if (uloop_interval_set(
                &loop.cpu_timer,
                (unsigned int)(config->monitor_cpu_usage_interval_microseconds / 1000U)
            ) != 0) {
            log_message(LOG_LEVEL_WARNING, "could not monitor CPU timer: %s", strerror(errno));
        }
    }
    if (config->log_to_file) {
        uint64_t buffer_milliseconds = config->log_file_buffer_timeout_microseconds / 1000U;

        if (uloop_interval_set(
                &loop.log_timer,
                (unsigned int)(buffer_milliseconds > 0U ? buffer_milliseconds : 1U)
            ) != 0 ||
            uloop_signal_add(&loop.log_export_signal) != 0 ||
            uloop_signal_add(&loop.log_reset_signal) != 0) {
            log_message(LOG_LEVEL_WARNING, "log maintenance degraded: %s", strerror(errno));
        }
    }
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
    if (loop.qdisc_events.registered) {
        (void)uloop_fd_delete(&loop.qdisc_events);
    }
    (void)uloop_interval_cancel(&loop.traffic_timer);
    (void)uloop_interval_cancel(&loop.reflector_health_timer);
    (void)uloop_interval_cancel(&loop.cpu_timer);
    (void)uloop_interval_cancel(&loop.log_timer);
    (void)uloop_signal_delete(&loop.log_export_signal);
    (void)uloop_signal_delete(&loop.log_reset_signal);
    uloop_done();
    uloop_handle_sigchld = previous_sigchld_handling;

done:
    for (index = 0U; index < health_count; index++) {
        health_cleanup(&loop.observation.reflector_health[index]);
    }
    netlink_close(&loop.observation.netlink);
    latency_close(&loop.observation.latency);
    controller_close(&loop.observation.controller);
    return loop.result;
}
