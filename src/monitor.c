#define _GNU_SOURCE

#include "monitor.h"

#include "common/constants.h"
#include "cake/cake.h"
#include "controller/controller.h"
#include "controller/reflector.h"
#include "platform/cpu.h"
#include "common/error.h"
#include "common/helpers.h"
#include "latency/latency.h"
#include "logging/log.h"
#include "platform/netlink.h"
#include "platform/traffic.h"

#include <libubox/list.h>
#include <libubox/uloop.h>
#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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
    uint64_t traffic_sample_id;
    bool cake_valid;
    bool traffic_valid;
    uint64_t next_cake_observation_microseconds;
};

static void observe_traffic(
    struct monitored_direction *direction,
    const struct timespec *timestamp
)
{
    struct traffic_sample sample;
    enum traffic_update_result update_result;

    direction->traffic_rate_bits_per_second = 0U;
    direction->traffic_valid = false;
    if (
        !direction->cake_valid ||
        !direction->cake.has_basic_stats
    ) {
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

    sample = (struct traffic_sample) {
        .bytes = direction->cake.bytes,
        .qdisc_handle = direction->cake.handle,
        .qdisc_parent = direction->cake.parent,
        .timestamp = *timestamp
    };
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
        direction->traffic_sample_id++;
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

    if (
        !observation->has_bandwidth ||
        observation->bandwidth_bits_per_second == 0U
    ) {
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
    char error[ERROR_SIZE] = { 0 };
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
    bool response_clock_failed;
    bool traffic_clock_failed;
    bool health_clock_failed;
    bool traffic_cadence_initialized;
    uint64_t traffic_cadence_microseconds;
    uint64_t last_reflector_replacement_microseconds;
    uint64_t last_reflector_comparison_microseconds;
    uint64_t last_reflector_response_microseconds;
    uint64_t last_pinger_restart_microseconds;
    uint64_t pinger_slot_origin_microseconds;
    uint64_t next_latency_attempt_microseconds;
    uint64_t pinger_grace_until_microseconds;
    struct controller_activity activity;
};

struct event_loop;

struct latency_watch {
    struct uloop_fd descriptor;
    struct event_loop *loop;
    size_t child_index;
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
    bool traffic_cadence_applied;
    struct uloop_fd qdisc_events;
    struct latency_watch latency_output[CONFIG_MAX_REFLECTORS];
    struct uloop_timeout latency_start_timer;
    int result;
};

static void handle_traffic_timer(struct uloop_interval *timer);
static bool schedule_irtt_child_start(struct event_loop *loop);

static void apply_traffic_cadence(struct event_loop *loop)
{
    if (!loop->observation.traffic_cadence_initialized ||
        loop->traffic_cadence_applied) {
        return;
    }
    if (uloop_interval_set(
        &loop->traffic_timer,
        traffic_interval_milliseconds(
            loop->observation.traffic_cadence_microseconds
        )
    ) != 0) {
        log_message(
            LOG_LEVEL_WARNING,
            "could not apply compensated traffic cadence: %s",
            strerror(errno)
        );
        return;
    }
    loop->traffic_cadence_applied = true;
}

static bool cake_ready(const struct observation_context *context)
{
    return context->download.cake_state == CAKE_OBSERVATION_AVAILABLE &&
        context->upload.cake_state == CAKE_OBSERVATION_AVAILABLE;
}

static bool wire_metadata_ready(const struct observation_context *context)
{
    return cake_ready(context) &&
        context->download.cake_valid &&
        context->upload.cake_valid &&
        context->download.cake.has_mtu &&
        context->upload.cake.has_mtu &&
        context->download.cake.has_bandwidth &&
        context->upload.cake.has_bandwidth;
}

static void update_serialization_compensation(
    struct observation_context *context
)
{
    if (!wire_metadata_ready(context)) {
        return;
    }
    controller_set_serialization_compensation(
        &context->controller,
        cake_max_wire_packet_bits(&context->download.cake),
        cake_max_wire_packet_bits(&context->upload.cake),
        context->controller.download.shaper_rate_bits_per_second,
        context->controller.upload.shaper_rate_bits_per_second
    );
}

static bool entropy_u32(uint32_t *value, void *context)
{
    (void)context;
    return getentropy(value, sizeof(*value)) == 0;
}

static const char *line_state_name(enum controller_line_state state)
{
    switch (state) {
    case CONTROLLER_LINE_UNKNOWN:
        return STATE_UNKNOWN;
    case CONTROLLER_LINE_BELOW_CAPACITY:
        return STATE_BELOW_CAPACITY;
    case CONTROLLER_LINE_SATURATED:
        return STATE_SATURATED;
    }

    return STATE_INVALID;
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
        return STATE_UNKNOWN;
    case CONTROLLER_CONGESTION_CLEAR:
        return STATE_CLEAR;
    case CONTROLLER_CONGESTION_DETECTED:
        return STATE_DETECTED;
    }

    return STATE_INVALID;
}

static void log_congestion_state(
    const char *direction,
    enum controller_congestion_state state,
    const struct latency_observation *latency
)
{
    int64_t round_trip_microseconds;
    int64_t baseline_microseconds;
    int64_t delay_microseconds;

    if (state == CONTROLLER_CONGESTION_UNKNOWN) {
        log_message(
            LOG_LEVEL_WARNING,
            "congestion observation unavailable: direction=%s",
            direction
        );
        return;
    }

    round_trip_microseconds = latency->download_owd_microseconds +
        latency->upload_owd_microseconds;
    baseline_microseconds = latency->download_owd_baseline_microseconds +
        latency->upload_owd_baseline_microseconds;
    delay_microseconds = round_trip_microseconds >= baseline_microseconds
        ? round_trip_microseconds - baseline_microseconds
        : 0;
    log_message(
        state == CONTROLLER_CONGESTION_DETECTED
            ? LOG_LEVEL_NOTICE
            : LOG_LEVEL_INFO,
        "congestion changed: direction=%s state=%s"
        " rtt=%" PRId64 " us baseline=%" PRId64 " us"
        " delta=%" PRId64 " us",
        direction,
        congestion_state_name(state),
        round_trip_microseconds,
        baseline_microseconds,
        delay_microseconds
    );
}

static const char *rate_reason_name(enum controller_rate_reason reason)
{
    switch (reason) {
    case CONTROLLER_RATE_UNCHANGED:
        return STATE_UNCHANGED;
    case CONTROLLER_RATE_INITIAL:
        return STATE_INITIAL;
    case CONTROLLER_RATE_CONGESTION:
        return STATE_CONGESTION;
    case CONTROLLER_RATE_HIGH_LOAD:
        return STATE_HIGH_LOAD;
    case CONTROLLER_RATE_RETURN_TO_BASE:
        return STATE_RETURN_TO_BASE;
    case CONTROLLER_RATE_RECONCILE:
        return STATE_RECONCILE;
    }

    return STATE_INVALID;
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
    unsigned int load,
    uint64_t connection_active_threshold,
    uint64_t high_load_threshold_percent,
    enum controller_congestion_state congestion
)
{
    const char *state;

    if (load > high_load_threshold_percent) {
        state = STATE_HIGH;
    } else if (traffic_rate > connection_active_threshold) {
        state = STATE_LOW;
    } else {
        state = STATE_IDLE;
    }

    (void)snprintf(
        condition,
        condition_size,
        "%s_%s%s",
        direction,
        state,
        congestion == CONTROLLER_CONGESTION_DETECTED ? BUFFERBLOAT_SUFFIX : EMPTY_STRING
    );
}

static void log_load_stats(
    const struct monitored_direction *download,
    const struct monitored_direction *upload
)
{
    const struct log_load_record record = {
        .download_achieved_rate_kbps =
            download->traffic_rate_bits_per_second / KILOBIT,
        .upload_achieved_rate_kbps =
            upload->traffic_rate_bits_per_second / KILOBIT,
        .cake_download_rate_kbps =
            download->cake.bandwidth_bits_per_second / KILOBIT,
        .cake_upload_rate_kbps =
            upload->cake.bandwidth_bits_per_second / KILOBIT
    };

    log_load(&record);
}

static void log_controller_stats(
    const struct config *config,
    const struct controller_direction_config *download_effective,
    const struct controller_direction_config *upload_effective,
    uint64_t high_load_threshold_percent,
    const struct controller_input *input,
    const struct controller_output *output,
    const struct latency_observation *latency,
    const char *reflector
)
{
    char download_condition[LOAD_CONDITION_SIZE];
    char upload_condition[LOAD_CONDITION_SIZE];
    uint64_t download_rate =
        output->download.rate_bits_per_second / KILOBIT;
    uint64_t upload_rate =
        output->upload.rate_bits_per_second / KILOBIT;
    unsigned int download_load;
    unsigned int upload_load;

    if (
        !config->output_processing_stats &&
        !config->output_summary_stats
    ) {
        return;
    }
    download_load = load_percent(
        input->download.traffic_rate_bits_per_second,
        input->download.cake_rate_bits_per_second
    );
    upload_load = load_percent(
        input->upload.traffic_rate_bits_per_second,
        input->upload.cake_rate_bits_per_second
    );

    load_condition(
        download_condition,
        sizeof(download_condition),
        DIRECTION_DOWNLOAD_SHORT,
        input->download.traffic_rate_bits_per_second,
        download_load,
        config->connection_active_threshold_bits_per_second,
        high_load_threshold_percent,
        output->download.congestion
    );
    load_condition(
        upload_condition,
        sizeof(upload_condition),
        DIRECTION_UPLOAD_SHORT,
        input->upload.traffic_rate_bits_per_second,
        upload_load,
        config->connection_active_threshold_bits_per_second,
        high_load_threshold_percent,
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
                input->download.traffic_rate_bits_per_second / KILOBIT,
            .upload_achieved_rate_kbps =
                input->upload.traffic_rate_bits_per_second / KILOBIT,
            .download_load_percent = download_load,
            .upload_load_percent = upload_load,
            .icmp_timestamp_microseconds = latency->timestamp_microseconds,
            .reflector = reflector,
            .sequence = latency->sequence,
            .download_owd_baseline_microseconds =
                latency->download_owd_baseline_microseconds,
            .download_owd_microseconds = latency->download_owd_microseconds,
            .download_owd_delta_ewma_microseconds =
                latency->download_owd_delta_ewma_microseconds,
            .download_owd_delta_microseconds =
                latency->download_owd_delta_microseconds,
            .download_adjust_delay_threshold_microseconds =
                download_effective->delay_threshold_microseconds,
            .upload_owd_baseline_microseconds =
                latency->upload_owd_baseline_microseconds,
            .upload_owd_microseconds = latency->upload_owd_microseconds,
            .upload_owd_delta_ewma_microseconds =
                latency->upload_owd_delta_ewma_microseconds,
            .upload_owd_delta_microseconds =
                latency->upload_owd_delta_microseconds,
            .upload_adjust_delay_threshold_microseconds =
                upload_effective->delay_threshold_microseconds,
            .download_sum_delays =
                output->download.delayed_sample_count,
            .download_average_owd_delta_microseconds =
                output->download.average_delay_microseconds,
            .download_maximum_adjust_up_threshold_microseconds =
                download_effective->average_delay_maximum_adjust_up_microseconds,
            .download_maximum_adjust_down_threshold_microseconds =
                download_effective->average_delay_maximum_adjust_down_microseconds,
            .upload_sum_delays = output->upload.delayed_sample_count,
            .upload_average_owd_delta_microseconds =
                output->upload.average_delay_microseconds,
            .upload_maximum_adjust_up_threshold_microseconds =
                upload_effective->average_delay_maximum_adjust_up_microseconds,
            .upload_maximum_adjust_down_threshold_microseconds =
                upload_effective->average_delay_maximum_adjust_down_microseconds,
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
                input->download.traffic_rate_bits_per_second / KILOBIT,
            .upload_achieved_rate_kbps =
                input->upload.traffic_rate_bits_per_second / KILOBIT,
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
    char error[ERROR_SIZE] = { 0 };
    enum cake_read_result read_result;

    if (output_cake_changes) {
        log_shaper(direction->interface, desired_rate / KILOBIT);
    }

    if (
        cake_set_bandwidth(
            netlink,
            direction->interface,
            &direction->cake,
            desired_rate,
            error,
            sizeof(error)
        ) != 0
    ) {
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
    if (
        read_result != CAKE_READ_FOUND || !verified.has_bandwidth ||
        verified.bandwidth_bits_per_second != desired_rate
    ) {
        log_message(
            LOG_LEVEL_WARNING,
            "CAKE bandwidth verification failed: direction=%s interface=%s"
            " desired_rate=%" PRIu64 " bit/s result=%s",
            direction->name,
            direction->interface,
            desired_rate,
            read_result == CAKE_READ_ERROR
                ? error
                : READBACK_MISMATCH
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
        .traffic_sample_id = direction->traffic_sample_id,
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
    struct observation_context *context,
    const struct config *config,
    const struct latency_observation *latency,
    const char *reflector
)
{
    struct controller_input input = {
        .download = direction_input(&context->download),
        .upload = direction_input(&context->upload),
        .download_latency = {
            .valid = true,
            .owd_delta_microseconds = latency->download_owd_delta_microseconds
        },
        .upload_latency = {
            .valid = true,
            .owd_delta_microseconds = latency->upload_owd_delta_microseconds
        },
        .timestamp_microseconds = 0U
    };
    struct controller_output output;
    const struct {
        struct monitored_direction *direction;
        const struct controller_direction_input *input;
        const struct controller_direction_output *output;
    } directions[] = {
        { &context->download, &input.download, &output.download },
        { &context->upload, &input.upload, &output.upload }
    };

    (void)read_clock_microseconds(CLOCK_MONOTONIC, &input.timestamp_microseconds);

    controller_update(
        &context->controller,
        &input,
        &output
    );
    for (size_t index = 0U; index < ARRAY_SIZE(directions); index++) {
        struct monitored_direction *direction = directions[index].direction;
        const struct controller_direction_output *decision = directions[index].output;

        if (decision->state_changed) {
            log_line_state(direction->name, decision->state, directions[index].input);
        }
        if (decision->congestion_changed) {
            log_congestion_state(direction->name, decision->congestion, latency);
        }
        /* The controller never requests changes for an observation-only link. */
        if (decision->rate_changed) {
            apply_bandwidth(
                &context->netlink,
                direction,
                decision->rate_bits_per_second,
                decision->rate_reason,
                config->output_cake_changes
            );
        }
    }
    update_serialization_compensation(context);
    log_controller_stats(
        config,
        &context->controller.download.config,
        &context->controller.upload.config,
        context->controller.config.high_load_threshold_percent,
        &input,
        &output,
        latency,
        reflector
    );
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
            (uint64_t)traffic_timestamp.tv_sec * MICROSECONDS_PER_SECOND +
            (uint64_t)traffic_timestamp.tv_nsec / NANOSECONDS_PER_MICROSECOND;
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
        update_serialization_compensation(context);
        if (!context->traffic_cadence_initialized && wire_metadata_ready(context)) {
            context->traffic_cadence_microseconds =
                traffic_compensated_interval_microseconds(
                    config->monitor_achieved_rates_interval_microseconds,
                    cake_max_wire_packet_bits(&context->download.cake),
                    config->base_download_rate_bits_per_second,
                    cake_max_wire_packet_bits(&context->upload.cake),
                    config->base_upload_rate_bits_per_second
                );
            context->traffic_cadence_initialized = true;
        }
        observe_traffic(&context->download, &traffic_timestamp);
        observe_traffic(&context->upload, &traffic_timestamp);
    }

    if (
        config->output_load_stats &&
        context->download.traffic_valid && context->upload.traffic_valid &&
        context->download.cake_valid && context->upload.cake_valid &&
        context->download.cake.has_bandwidth &&
        context->upload.cake.has_bandwidth
    ) {
        log_load_stats(&context->download, &context->upload);
    }
}

static bool ensure_latency_open(
    struct observation_context *context,
    const struct config *config
)
{
    const char *targets[CONFIG_MAX_REFLECTORS];
    char error[ERROR_SIZE] = { 0 };
    size_t target_count = (size_t)config->no_pingers;
    size_t index;
    uint64_t timestamp_microseconds;
    uint64_t first_start_microseconds;
    int result;

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
    if (strcmp(config->pinger_method, PINGER_METHOD_IRTT) == 0) {
        uint64_t elapsed = timestamp_microseconds -
            context->pinger_slot_origin_microseconds;
        uint64_t remainder = elapsed %
            config->reflector_ping_interval_microseconds;

        first_start_microseconds = timestamp_microseconds +
            config->reflector_ping_interval_microseconds - remainder;
        result = latency_open_irtt(
            &context->latency,
            targets,
            target_count,
            config->reflector_ping_interval_microseconds,
            config->irtt_session_duration_minutes,
            config->ping_extra_args,
            config->ping_prefix_string,
            first_start_microseconds,
            error,
            sizeof(error)
        );
    } else {
        result = latency_open(
            &context->latency,
            config->interface,
            targets,
            target_count,
            config->reflector_ping_interval_microseconds,
            config->ping_extra_args,
            config->ping_prefix_string,
            error,
            sizeof(error)
        );
    }
    if (result != 0) {
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

    if (strcmp(config->pinger_method, PINGER_METHOD_IRTT) == 0) {
        log_message(
            context->latency_observation_failed
                ? LOG_LEVEL_NOTICE
                : LOG_LEVEL_INFO,
            context->latency_observation_failed
                ? "latency observation recovered: targets=%zu pinger=irtt"
                : "latency observation initialized: targets=%zu pinger=irtt",
            target_count
        );
    } else {
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
    }
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
        if (
            strcmp(
                config->reflectors[context->reflector_order[index]],
                target
            ) == 0
        ) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool receive_latency_samples(
    struct observation_context *context,
    const struct config *config,
    size_t child_index
)
{
    for (;;) {
        struct latency_observation observation;
        struct latency_sample sample;
        char error[ERROR_SIZE] = { 0 };
        bool stale;
        uint64_t processing_realtime_microseconds;
        uint64_t processing_monotonic_microseconds;
        uint64_t response_monotonic_microseconds;
        enum latency_probe_result result = latency_receive_child(
            &context->latency,
            child_index,
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
        if (result == LATENCY_PROBE_RESTART) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Restarting irtt pinger: pinger=%zu (%s)",
                child_index,
                error
            );
            return true;
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

        if (
            !read_clock_microseconds(
                CLOCK_REALTIME,
                &processing_realtime_microseconds
            ) ||
            !read_clock_microseconds(
                CLOCK_MONOTONIC,
                &processing_monotonic_microseconds
            )
        ) {
            if (!context->response_clock_failed) {
                log_message(
                    LOG_LEVEL_WARNING,
                    "latency observation degraded: response clock failed: %s",
                    strerror(errno)
                );
            }
            context->response_clock_failed = true;
            continue;
        }
        if (context->response_clock_failed) {
            log_message(LOG_LEVEL_NOTICE, "latency response clock recovered");
            context->response_clock_failed = false;
        }
        low_load = direction_has_low_load(
            &context->download,
            context->controller.config.high_load_threshold_percent
        ) && direction_has_low_load(
            &context->upload,
            context->controller.config.high_load_threshold_percent
        );

        tracker_update(
            &context->latency_trackers[
                context->reflector_order[reflector_index]
            ],
            &sample,
            &observation
        );

        tracker_update_delta_ewma(
            &context->latency_trackers[
                context->reflector_order[reflector_index]
            ],
            low_load,
            &observation
        );
        response_timestamp(
            processing_realtime_microseconds,
            processing_monotonic_microseconds,
            sample.timestamp_microseconds,
            &response_monotonic_microseconds,
            &stale
        );
        context->last_reflector_response_microseconds =
            response_monotonic_microseconds;
        health_record_response(
            &context->reflector_health[reflector_index],
            response_monotonic_microseconds
        );
        if (stale) {
            log_message(
                LOG_LEVEL_DEBUG,
                "processed response from [%s] that is > 500ms old. Skipping.",
                sample.target
            );
            continue;
        }

        update_controller(
            context,
            config,
            &observation,
            sample.target
        );
    }
}

static void close_latency(struct event_loop *loop)
{
    size_t index;

    (void)uloop_timeout_cancel(&loop->latency_start_timer);
    for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
        if (loop->latency_output[index].descriptor.registered) {
            (void)uloop_fd_delete(&loop->latency_output[index].descriptor);
        }
        loop->latency_output[index].descriptor.fd = -1;
    }
    latency_close(&loop->observation.latency);
}

static void defer_latency_retry(struct event_loop *loop)
{
    uint64_t timestamp_microseconds;

    close_latency(loop);
    if (read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
        loop->observation.next_latency_attempt_microseconds =
            timestamp_microseconds +
            loop->config->interface_up_check_interval_microseconds;
    }
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
    /* Preserve the sample ID: the controller retains its last consumed ID. */
    direction->traffic_valid = false;
    direction->traffic_state = TRAFFIC_OBSERVATION_UNKNOWN;
    direction->traffic_rate_bits_per_second = 0U;
    traffic_init(&direction->traffic_monitor);
}

static void process_qdisc_event(
    const struct qdisc_event *event,
    void *context
)
{
    struct event_loop *loop = context;
    struct monitored_direction *direction = event_direction(loop, event);

    if (direction == NULL) {
        return;
    }
    if (event->type == QDISC_REMOVED) {
        if (
            direction->cake_state != CAKE_OBSERVATION_AVAILABLE ||
            direction->cake.handle != event->handle
        ) {
            return;
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
        return;
    }

    /* Bandwidth changes notify RTM_NEWQDISC with the existing handle. */
    if (
        direction->cake_state == CAKE_OBSERVATION_AVAILABLE &&
        direction->cake.handle == event->handle
    ) {
        return;
    }
    if (direction->cake_state == CAKE_OBSERVATION_AVAILABLE) {
        direction->cake_state = CAKE_OBSERVATION_NOT_FOUND;
    }
    direction->cake_valid = false;
    direction->next_cake_observation_microseconds = 0U;
    reset_traffic_observation(direction);
    loop->qdisc_refresh = true;
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
    char error[ERROR_SIZE] = { 0 };

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
    struct latency_watch *watch = __extension__ container_of(
        descriptor,
        struct latency_watch,
        descriptor
    );
    struct event_loop *loop = watch->loop;

    (void)events;
    if (!receive_latency_samples(
        &loop->observation,
        loop->config,
        watch->child_index
    )) {
        defer_latency_retry(loop);
    } else if (latency_child_descriptor(
        &loop->observation.latency,
        watch->child_index
    ) < 0) {
        if (watch->descriptor.registered) {
            (void)uloop_fd_delete(&watch->descriptor);
        }
        watch->descriptor.fd = -1;
        (void)schedule_irtt_child_start(loop);
    }
}

static bool watch_started_latency_children(struct event_loop *loop)
{
    size_t index;
    int saved_errno;

    for (
        index = 0U;
        index < latency_child_count(&loop->observation.latency);
        index++
    ) {
        struct latency_watch *watch = &loop->latency_output[index];
        int descriptor = latency_child_descriptor(
            &loop->observation.latency,
            index
        );

        if (watch->descriptor.registered || descriptor < 0) {
            continue;
        }
        watch->descriptor.fd = descriptor;
        if (uloop_fd_add(&watch->descriptor, ULOOP_READ | ULOOP_ERROR_CB) != 0) {
            saved_errno = errno;
            log_message(
                LOG_LEVEL_WARNING,
                "latency observation degraded: could not monitor %s output: %s",
                loop->config->pinger_method,
                strerror(saved_errno)
            );
            loop->observation.latency_observation_failed = true;
            close_latency(loop);
            return false;
        }
    }
    return true;
}

static bool schedule_irtt_child_start(struct event_loop *loop)
{
    char error[ERROR_SIZE] = { 0 };
    uint64_t timestamp_microseconds;
    uint64_t next_start_microseconds;
    uint64_t delay_microseconds;
    uint64_t delay_milliseconds;

    if (!latency_irtt_start_pending(&loop->observation.latency)) {
        (void)uloop_timeout_cancel(&loop->latency_start_timer);
        return true;
    }
    if (!read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
        log_message(
            LOG_LEVEL_WARNING,
            "latency observation degraded: IRTT start clock failed: %s",
            strerror(errno)
        );
        loop->observation.latency_observation_failed = true;
        defer_latency_retry(loop);
        return false;
    }
    if (
        latency_start_irtt_children(
            &loop->observation.latency,
            timestamp_microseconds,
            error,
            sizeof(error)
        ) != 0
    ) {
        log_message(
            LOG_LEVEL_WARNING,
            "latency observation degraded: %s",
            error
        );
        loop->observation.latency_observation_failed = true;
        defer_latency_retry(loop);
        return false;
    }
    if (!watch_started_latency_children(loop)) {
        return false;
    }
    if (!latency_irtt_start_pending(&loop->observation.latency)) {
        return true;
    }

    next_start_microseconds = latency_irtt_next_start_microseconds(
        &loop->observation.latency
    );
    delay_microseconds = next_start_microseconds > timestamp_microseconds
        ? next_start_microseconds - timestamp_microseconds
        : 0U;
    delay_milliseconds =
        delay_microseconds / MICROSECONDS_PER_MILLISECOND;
    if (delay_microseconds % MICROSECONDS_PER_MILLISECOND != 0U) {
        delay_milliseconds++;
    }
    if (delay_milliseconds > (uint64_t)INT_MAX) {
        delay_milliseconds = (uint64_t)INT_MAX;
    }
    if (
        uloop_timeout_set(
            &loop->latency_start_timer,
            (int)delay_milliseconds
        ) != 0
    ) {
        log_message(
            LOG_LEVEL_WARNING,
            "latency observation degraded: could not schedule IRTT start: %s",
            strerror(errno)
        );
        loop->observation.latency_observation_failed = true;
        defer_latency_retry(loop);
        return false;
    }
    return true;
}

static void handle_latency_start(struct uloop_timeout *timer)
{
    struct event_loop *loop = __extension__ container_of(
        timer,
        struct event_loop,
        latency_start_timer
    );

    (void)schedule_irtt_child_start(loop);
}

static bool watch_latency(struct event_loop *loop)
{
    if (!ensure_latency_open(&loop->observation, loop->config)) {
        return false;
    }
    if (strcmp(loop->config->pinger_method, PINGER_METHOD_IRTT) == 0) {
        return schedule_irtt_child_start(loop);
    }
    return watch_started_latency_children(loop);
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
    if (
        config->adjust_download &&
        context->download.cake_valid
    ) {
        apply_bandwidth(
            &context->netlink,
            &context->download,
            config->minimum_download_rate_bits_per_second,
            CONTROLLER_RATE_RECONCILE,
            config->output_cake_changes
        );
    }
    if (
        config->adjust_upload &&
        context->upload.cake_valid
    ) {
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
    static const char *const names[] = {
        STATE_RUNNING_UPPER,
        STATE_IDLE_UPPER,
        STATE_STALL_UPPER
    };
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
            (double)activity_config.stall_timeout_microseconds /
            (double)MICROSECONDS_PER_SECOND
        );
        log_message(
            LOG_LEVEL_DEBUG,
            "load check is: (( %" PRIu64 " kbps > %" PRIu64
            " kbps for download && %" PRIu64 " kbps > %" PRIu64
            " kbps for upload ))",
            context->download.traffic_rate_bits_per_second / KILOBIT,
            config->connection_stall_threshold_bits_per_second / KILOBIT,
            context->upload.traffic_rate_bits_per_second / KILOBIT,
            config->connection_stall_threshold_bits_per_second / KILOBIT
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
            (double)activity_config.global_timeout_microseconds /
            (double)MICROSECONDS_PER_SECOND
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

static void replace_active_reflector(
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
        return;
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
    apply_traffic_cadence(loop);
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
    /* cpu_read initializes only the counters actually returned by the kernel. */
    struct cpu_sample sample;
    unsigned int usage[CPU_MAX_COUNT];
    char error[ERROR_SIZE] = { 0 };

    if (cpu_read(PROC_STAT_PATH, &sample, error, sizeof(error)) != 0) {
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
    char export_path[LOG_EXPORT_PATH_SIZE];

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
            /* Keep both upstream columns even though fping shares the delay. */
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

        if (
            comparison->sum_owd_baselines_delta_microseconds >
            loop->config
                ->reflector_sum_owd_baselines_delta_threshold_microseconds
        ) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Warning: reflector: %s sum_owd_baselines_us exceeds the"
                " minimum by set threshold.",
                reflector
            );
        } else if (
            (uint64_t)comparison->download_delta_ewma_delta_microseconds >
            loop->config->reflector_owd_delta_ewma_delta_threshold_microseconds
        ) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Warning: reflector: %s dl_owd_delta_ewma_us exceeds the"
                " minimum by set threshold.",
                reflector
            );
        } else if (
            (uint64_t)comparison->upload_delta_ewma_delta_microseconds >
            loop->config->reflector_owd_delta_ewma_delta_threshold_microseconds
        ) {
            log_message(
                LOG_LEVEL_DEBUG,
                "Warning: reflector: %s ul_owd_delta_ewma_us exceeds the"
                " minimum by set threshold.",
                reflector
            );
        } else {
            continue;
        }

        replace_active_reflector(loop, index, timestamp_microseconds);
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
        loop->config->reflector_replacement_interval_minutes *
            MICROSECONDS_PER_MINUTE;
    uint64_t comparison_interval_microseconds =
        loop->config->reflector_comparison_interval_minutes *
            MICROSECONDS_PER_MINUTE;
    size_t pinger;

    if (
        interval_elapsed(
            timestamp_microseconds,
            loop->observation.last_reflector_replacement_microseconds,
            replacement_interval_microseconds
        )
    ) {
        loop->observation.last_reflector_replacement_microseconds =
            timestamp_microseconds;
        if (!random_below(
            (size_t)loop->config->no_pingers,
            entropy_u32,
            NULL,
            &pinger
        )) {
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
        replace_active_reflector(
            loop,
            pinger,
            timestamp_microseconds
        );
        return true;
    }

    if (
        interval_elapsed(
            timestamp_microseconds,
            loop->observation.last_reflector_comparison_microseconds,
            comparison_interval_microseconds
        )
    ) {
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
                (double)MICROSECONDS_PER_SECOND
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
                replace_active_reflector(
                    loop,
                    index,
                    timestamp_microseconds
                );
                reflector_replaced = true;
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
            THOUSAND
        ),
        .rate_maximum_adjust_down_bufferbloat_per_thousand = rounded_divide(
            config->shaper_rate_maximum_adjust_down_bufferbloat_per_million,
            THOUSAND
        ),
        .rate_minimum_adjust_up_high_load_per_thousand = rounded_divide(
            config->shaper_rate_minimum_adjust_up_load_high_per_million,
            THOUSAND
        ),
        .rate_maximum_adjust_up_high_load_per_thousand = rounded_divide(
            config->shaper_rate_maximum_adjust_up_load_high_per_million,
            THOUSAND
        ),
        .rate_adjust_down_low_load_per_thousand = rounded_divide(
            config->shaper_rate_adjust_down_load_low_per_million,
            THOUSAND
        ),
        .rate_adjust_up_low_load_per_thousand = rounded_divide(
            config->shaper_rate_adjust_up_load_low_per_million,
            THOUSAND
        ),
        .high_load_threshold_percent = rounded_divide(
            config->high_load_threshold_per_million,
            FACTOR_PER_PERCENT
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
                .name = DIRECTION_DOWNLOAD,
                .interface = config->ingress_interface,
                .cake_state = CAKE_OBSERVATION_UNKNOWN,
                .traffic_state = TRAFFIC_OBSERVATION_UNKNOWN
            },
            .upload = {
                .name = DIRECTION_UPLOAD,
                .interface = config->interface,
                .cake_state = CAKE_OBSERVATION_UNKNOWN,
                .traffic_state = TRAFFIC_OBSERVATION_UNKNOWN
            }
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
        .latency_start_timer = {
            .cb = handle_latency_start
        },
        .result = -1
    };
    bool previous_sigchld_handling = uloop_handle_sigchld;
    const struct {
        struct uloop_interval *timer;
        uint64_t interval_microseconds;
        const char *name;
    } required_timers[] = {
        {
            &loop.traffic_timer,
            config->monitor_achieved_rates_interval_microseconds,
            TIMER_TRAFFIC
        },
        {
            &loop.reflector_health_timer,
            config->reflector_health_check_interval_microseconds,
            TIMER_REFLECTOR_HEALTH
        }
    };
    int run_status;
    uint64_t start_microseconds;
    size_t health_count = 0U;
    size_t index;

    for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
        loop.latency_output[index].descriptor.cb = handle_latency_output;
        loop.latency_output[index].descriptor.fd = -1;
        loop.latency_output[index].loop = &loop;
        loop.latency_output[index].child_index = index;
    }

    latency_init(&loop.observation.latency);
    /* The aggregate initializer has zeroed the remaining monitor state. */
    if (
        controller_init(
            &loop.observation.controller,
            &controller_config
        ) != 0
    ) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not initialize controller: %s",
            strerror(errno)
        );
        goto done;
    }
    for (index = 0U; index < (size_t)config->reflector_count; index++) {
        if (
            tracker_init(
                &loop.observation.latency_trackers[index],
                &latency_tracker_config
            ) != 0
        ) {
            log_message(
                LOG_LEVEL_ERROR,
                "could not initialize latency tracker: %s",
                strerror(errno)
            );
            goto done;
        }
        loop.observation.reflector_order[index] = index;
    }
    log_message(LOG_LEVEL_DEBUG, "Randomizing reflectors.");
    if (config->randomize_reflectors) {
        size_t randomized_order[CONFIG_MAX_REFLECTORS];
        size_t reflector_count = (size_t)config->reflector_count;

        memcpy(
            randomized_order,
            loop.observation.reflector_order,
            reflector_count * sizeof(*randomized_order)
        );
        if (!shuffle(
            randomized_order,
            reflector_count,
            entropy_u32,
            NULL
        )) {
            log_message(
                LOG_LEVEL_WARNING,
                "could not randomize reflectors: %s; using configured order",
                strerror(errno)
            );
        } else {
            memcpy(
                loop.observation.reflector_order,
                randomized_order,
                reflector_count * sizeof(*randomized_order)
            );
        }
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
    loop.observation.pinger_slot_origin_microseconds = start_microseconds;
    loop.observation.activity.state = CONTROLLER_RUNNING;
    for (index = 0U; index < (size_t)config->no_pingers; index++) {
        if (
            health_init(
                &loop.observation.reflector_health[index],
                &reflector_health_config,
                start_microseconds
            ) != 0
        ) {
            log_message(
                LOG_LEVEL_ERROR,
                "could not initialize reflector health: %s",
                strerror(errno)
            );
            goto done;
        }
        health_count++;
    }

    /* latency.c owns and reaps pinger children; uloop must not consume them. */
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
        char error[ERROR_SIZE] = { 0 };

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
        if (
            uloop_interval_set(
                required_timers[index].timer,
                (unsigned int)(
                    required_timers[index].interval_microseconds /
                    MICROSECONDS_PER_MILLISECOND
                )
            ) != 0
        ) {
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
    apply_traffic_cadence(&loop);
    if (
        config->output_cpu_stats ||
        config->output_cpu_raw_stats
    ) {
        observe_cpu(&loop, false);
        if (
            uloop_interval_set(
                &loop.cpu_timer,
                (unsigned int)(
                    config->monitor_cpu_usage_interval_microseconds /
                    MICROSECONDS_PER_MILLISECOND
                )
            ) != 0
        ) {
            log_message(LOG_LEVEL_WARNING, "could not monitor CPU timer: %s", strerror(errno));
        }
    }
    if (config->log_to_file) {
        uint64_t buffer_milliseconds =
            (config->log_file_buffer_timeout_microseconds + MILLISECOND - 1U) /
            MILLISECOND;
        uint64_t log_timer_milliseconds = buffer_milliseconds;

        if (
            log_timer_milliseconds == 0U &&
            config->log_file_max_time_minutes > 0U
        ) {
            uint64_t maximum_age_milliseconds =
                config->log_file_max_time_minutes * (MINUTE / MILLISECOND);

            log_timer_milliseconds = maximum_age_milliseconds < UINT_MAX
                ? maximum_age_milliseconds + 1U
                : UINT_MAX;
        }
        if (
            log_timer_milliseconds > 0U &&
            uloop_interval_set(
                &loop.log_timer,
                (unsigned int)log_timer_milliseconds
            ) != 0
        ) {
            log_message(LOG_LEVEL_WARNING, "log timer degraded: %s", strerror(errno));
        }
        if (uloop_signal_add(&loop.log_export_signal) != 0) {
            log_message(LOG_LEVEL_WARNING, "log export signal degraded: %s", strerror(errno));
        }
        if (uloop_signal_add(&loop.log_reset_signal) != 0) {
            log_message(LOG_LEVEL_WARNING, "log reset signal degraded: %s", strerror(errno));
        }
    }
    (void)watch_latency(&loop);
    run_status = uloop_run();
    if (
        run_status == SIGINT ||
        run_status == SIGTERM
    ) {
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
    controller_close(&loop.observation.controller);
    return loop.result;
}
