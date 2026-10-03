#define _GNU_SOURCE

#include "monitor/loop.h"

#include "common/helpers.h"
#include "common/utils.h"
#include "logging/log.h"

#include <libubox/utils.h>

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

static bool entropy_u32(uint32_t *value, void *context)
{
    (void)context;
    return getentropy(value, sizeof(*value)) == 0;
}

void reset_reflector_health(
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

void handle_reflector_health_timer(struct uloop_interval *timer)
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

int start_reflectors(
    struct observation_context *context,
    const struct config *config,
    uint64_t start_microseconds
)
{
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
    size_t index;

    for (index = 0U; index < (size_t)config->reflector_count; index++) {
        if (
            tracker_init(
                &context->latency_trackers[index],
                &latency_tracker_config
            ) != 0
        ) {
            log_message(
                LOG_LEVEL_ERROR,
                "could not initialize latency tracker: %s",
                strerror(errno)
            );
            return -1;
        }
        context->reflector_order[index] = index;
    }
    log_message(LOG_LEVEL_DEBUG, "Randomizing reflectors.");
    if (config->randomize_reflectors) {
        size_t randomized_order[CONFIG_MAX_REFLECTORS];
        size_t reflector_count = (size_t)config->reflector_count;

        memcpy(
            randomized_order,
            context->reflector_order,
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
                context->reflector_order,
                randomized_order,
                reflector_count * sizeof(*randomized_order)
            );
        }
    }
    for (index = 0U; index < (size_t)config->no_pingers; index++) {
        if (
            health_init(
                &context->reflector_health[index],
                &reflector_health_config,
                start_microseconds
            ) != 0
        ) {
            log_message(
                LOG_LEVEL_ERROR,
                "could not initialize reflector health: %s",
                strerror(errno)
            );
            return -1;
        }
    }
    return 0;
}

/* Health windows that were never allocated are NULL in the zeroed context. */
void stop_reflectors(
    struct observation_context *context,
    const struct config *config
)
{
    size_t index;

    for (index = 0U; index < (size_t)config->no_pingers; index++) {
        health_cleanup(&context->reflector_health[index]);
    }
}
