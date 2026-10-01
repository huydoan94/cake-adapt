#define _GNU_SOURCE

#include "monitor/monitor.h"
#include "monitor/loop.h"

#include "common/helpers.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <string.h>

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
            grant_pinger_setup_grace(context, config, timestamp_microseconds);
            context->next_latency_attempt_microseconds = 0U;
            (void)watch_latency(loop);
        }
    }
    if (output.restart_pingers) {
        log_message(LOG_LEVEL_DEBUG, "Restarting pingers.");
        restart_latency(loop, timestamp_microseconds);
    }
}

void handle_traffic_timer(
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
        loop->observation.pingers_suspended = true;
        return;
    }
    if (read_clock_microseconds(CLOCK_MONOTONIC, &timestamp_microseconds)) {
        /* Pingers stopped while CAKE was missing restart like after IDLE. */
        if (loop->observation.pingers_suspended) {
            grant_pinger_setup_grace(
                &loop->observation,
                loop->config,
                timestamp_microseconds
            );
            loop->observation.pingers_suspended = false;
        }
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

static void watch_cpu(struct event_loop *loop)
{
    const struct config *config = loop->config;

    if (!config->output_cpu_stats && !config->output_cpu_raw_stats) {
        return;
    }
    observe_cpu(loop, false);
    if (
        uloop_interval_set(
            &loop->cpu_timer,
            (unsigned int)(
                config->monitor_cpu_usage_interval_microseconds /
                MICROSECONDS_PER_MILLISECOND
            )
        ) != 0
    ) {
        log_message(LOG_LEVEL_WARNING, "could not monitor CPU timer: %s", strerror(errno));
    }
}

static void watch_log_maintenance(struct event_loop *loop)
{
    const struct config *config = loop->config;
    uint64_t log_timer_milliseconds;

    if (!config->log_to_file) {
        return;
    }
    log_timer_milliseconds =
        milliseconds_rounded_up(config->log_file_buffer_timeout_microseconds);
    /* Without a buffer timer, still wake up to rotate at the maximum age. */
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
            &loop->log_timer,
            (unsigned int)log_timer_milliseconds
        ) != 0
    ) {
        log_message(LOG_LEVEL_WARNING, "log timer degraded: %s", strerror(errno));
    }
    if (uloop_signal_add(&loop->log_export_signal) != 0) {
        log_message(LOG_LEVEL_WARNING, "log export signal degraded: %s", strerror(errno));
    }
    if (uloop_signal_add(&loop->log_reset_signal) != 0) {
        log_message(LOG_LEVEL_WARNING, "log reset signal degraded: %s", strerror(errno));
    }
}

int monitor_run(const struct config *config)
{
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
            .fd = -1
        },
        .result = -1
    };
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
    size_t index;

    /* The aggregate initializer has zeroed the remaining monitor state. */
    prepare_pingers(&loop);
    if (start_controller(&loop.observation.controller, config) != 0) {
        goto done;
    }
    if (!read_clock_microseconds(CLOCK_MONOTONIC, &start_microseconds)) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not initialize reflector health clock: %s",
            strerror(errno)
        );
        goto done;
    }
    if (start_reflectors(&loop.observation, config, start_microseconds) != 0) {
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

    /* uloop reaps pinger children and reports each exit to handle_pinger_exit(). */
    if (uloop_init() != 0) {
        log_message(
            LOG_LEVEL_ERROR,
            "could not initialize event loop: %s",
            strerror(errno)
        );
        goto done;
    }

    if (watch_qdisc_events(&loop) != 0) {
        goto uloop_done;
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
    watch_cpu(&loop);
    watch_log_maintenance(&loop);
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
    stop_pingers_now(&loop);
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

done:
    stop_reflectors(&loop.observation, config);
    netlink_close(&loop.observation.netlink);
    controller_close(&loop.observation.controller);
    return loop.result;
}
