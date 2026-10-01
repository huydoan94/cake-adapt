#include "config/config.h"
#include "common/constants.h"
#include "common/error.h"
#include "latency/latency.h"

#include <limits.h>
#include <string.h>

/*
 * Current libuci headers contain inline helpers that trigger -Wsign-conversion.
 * Keep strict conversion warnings for cake-adapt while isolating that external
 * header warning.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

static int validate_rate_range(
    bool adjust,
    uint64_t minimum,
    uint64_t base,
    uint64_t maximum,
    const char *direction,
    char *error,
    size_t error_size
)
{
    if (
        !adjust &&
        minimum == 0U &&
        base == 0U &&
        maximum == 0U
    ) {
        return 0;
    }
    if (
        minimum == 0U ||
        base == 0U ||
        maximum == 0U
    ) {
        error_set(
            error,
            error_size,
            "%s min/base/max rates are required together",
            direction
        );
        return -1;
    }
    if (
        minimum > base ||
        base > maximum
    ) {
        error_set(
            error,
            error_size,
            "%s rates must satisfy minimum <= base <= maximum",
            direction
        );
        return -1;
    }
    /* The controller works in whole kbit/s, which CAKE can represent exactly. */
    if (
        minimum % KILOBIT != 0U ||
        base % KILOBIT != 0U ||
        maximum % KILOBIT != 0U
    ) {
        error_set(error, error_size, "%s shaper rates must be whole kbit/s", direction);
        return -1;
    }
    return 0;
}

static int validate_reflectors(
    const struct config *config,
    char *error,
    size_t error_size
)
{
    uint64_t index;
    uint64_t comparison;

    if (config->reflector_count == 0U) {
        error_set(error, error_size, "at least one reflector is required");
        return -1;
    }
    if (
        config->no_pingers == 0U ||
        config->no_pingers > config->reflector_count
    ) {
        error_set(
            error,
            error_size,
            "option 'no_pingers' must be between 1 and the reflector count"
        );
        return -1;
    }
    for (index = 0U; index < config->reflector_count; index++) {
        if (!target_is_valid(config->reflectors[index])) {
            error_set(error, error_size, "invalid reflector '%s'", config->reflectors[index]);
            return -1;
        }
        for (comparison = index + 1U; comparison < config->reflector_count; comparison++) {
            if (strcmp(config->reflectors[index], config->reflectors[comparison]) == 0) {
                error_set(error, error_size, "duplicate reflector '%s'", config->reflectors[index]);
                return -1;
            }
        }
    }
    return 0;
}

static int validate_latency_config(
    const struct config *config,
    char *error,
    size_t error_size
)
{
    if (
        !config->enabled &&
        !config->adjust_download &&
        !config->adjust_upload
    ) {
        return 0;
    }
    if (
        strcmp(config->pinger_method, PINGER_METHOD_FPING) != 0 &&
        strcmp(config->pinger_method, PINGER_METHOD_IRTT) != 0
    ) {
        error_set(error, error_size, "option 'pinger_method' must be 'fping' or 'irtt'");
        return -1;
    }
    if (
        strcmp(config->pinger_method, PINGER_METHOD_IRTT) == 0 &&
        config->irtt_session_duration_minutes == 0U
    ) {
        error_set(error, error_size, "option 'irtt_session_duration_m' must be positive for irtt");
        return -1;
    }
    /* Loading bounds the reflector list; validation bounds no_pingers by it. */
    if (validate_reflectors(config, error, error_size) != 0) {
        return -1;
    }
    if (
        config->reflector_ping_interval_microseconds /
            config->no_pingers < MILLISECOND
    ) {
        error_set(
            error,
            error_size,
            "option 'reflector_ping_interval_s' must provide at least"
            " 1 ms per active reflector"
        );
        return -1;
    }
    if (config->monitor_achieved_rates_interval_microseconds == 0U) {
        error_set(
            error,
            error_size,
            "option 'monitor_achieved_rates_interval_ms' must be positive"
        );
        return -1;
    }
    if (
        config->monitor_achieved_rates_interval_microseconds % MILLISECOND != 0U
    ) {
        error_set(
            error,
            error_size,
            "option 'monitor_achieved_rates_interval_ms' must be a whole"
            " number of milliseconds"
        );
        return -1;
    }
    if (
        config->monitor_achieved_rates_interval_microseconds / MILLISECOND >
        UINT_MAX
    ) {
        error_set(
            error,
            error_size,
            "option 'monitor_achieved_rates_interval_ms' is too large"
        );
        return -1;
    }
    if (
        config->bufferbloat_detection_window == 0U ||
        config->bufferbloat_detection_window > UINT_MAX
    ) {
        error_set(
            error,
            error_size,
            "option 'bufferbloat_detection_window' must be between 1 and %u",
            UINT_MAX
        );
        return -1;
    }
    if (
        config->bufferbloat_detection_threshold >
        config->bufferbloat_detection_window
    ) {
        error_set(
            error,
            error_size,
            "option 'bufferbloat_detection_thr' cannot be greater than"
            " 'bufferbloat_detection_window'"
        );
        return -1;
    }
    if (
        config->alpha_baseline_increase_per_million > MILLION ||
        config->alpha_baseline_decrease_per_million > MILLION ||
        config->alpha_delta_ewma_per_million > MILLION
    ) {
        error_set(
            error,
            error_size,
            "alpha options must be between 0 and 1"
        );
        return -1;
    }
    if (
        config->reflector_health_check_interval_microseconds == 0U ||
        config->reflector_response_deadline_microseconds == 0U
    ) {
        error_set(
            error,
            error_size,
            "reflector health interval and response deadline must be positive"
        );
        return -1;
    }
    if (
        config->reflector_health_check_interval_microseconds % MILLISECOND != 0U ||
        config->reflector_health_check_interval_microseconds / MILLISECOND >
            UINT_MAX
    ) {
        error_set(
            error,
            error_size,
            "option 'reflector_health_check_interval_s' must be a whole"
            " number of milliseconds no greater than %u",
            UINT_MAX
        );
        return -1;
    }
    if (
        config->reflector_misbehaving_detection_window == 0U ||
        config->reflector_misbehaving_detection_window > SIZE_MAX ||
        config->reflector_misbehaving_detection_threshold == 0U ||
        config->reflector_misbehaving_detection_threshold >
            config->reflector_misbehaving_detection_window
    ) {
        error_set(
            error,
            error_size,
            "reflector offence threshold must be between 1 and its window"
        );
        return -1;
    }
    if (
        config->reflector_replacement_interval_minutes >
            UINT64_MAX / MICROSECONDS_PER_MINUTE ||
        config->reflector_comparison_interval_minutes >
            UINT64_MAX / MICROSECONDS_PER_MINUTE
    ) {
        error_set(
            error,
            error_size,
            "reflector replacement and comparison intervals are too large"
        );
        return -1;
    }
    if (
        config->stall_detection_threshold == 0U ||
        config->stall_detection_threshold >
            UINT64_MAX /
                (config->reflector_ping_interval_microseconds /
                    config->no_pingers) ||
        config->global_ping_response_timeout_microseconds == 0U ||
        config->interface_up_check_interval_microseconds == 0U
    ) {
        error_set(
            error,
            error_size,
            "stall, global ping timeout and interface retry settings must be"
            " positive and representable"
        );
        return -1;
    }
    if (
        (config->output_cpu_stats || config->output_cpu_raw_stats) &&
        (config->monitor_cpu_usage_interval_microseconds == 0U ||
            config->monitor_cpu_usage_interval_microseconds % MILLISECOND != 0U ||
            config->monitor_cpu_usage_interval_microseconds / MILLISECOND > UINT_MAX)
    ) {
        error_set(
            error,
            error_size,
            "CPU monitoring interval must be a positive whole number of milliseconds no greater than %u",
            UINT_MAX
        );
        return -1;
    }
    if (
        config->log_file_max_time_minutes > UINT64_MAX / MICROSECONDS_PER_MINUTE ||
        config->log_file_max_size_kilobytes > UINT64_MAX / KIBIBYTE ||
        config->log_file_buffer_timeout_microseconds / MILLISECOND > UINT_MAX ||
        config->reflector_ping_interval_microseconds > UINT64_MAX / 2U
    ) {
        error_set(error, error_size, "logging or pinger intervals are too large");
        return -1;
    }
    if (
        config->enable_sleep_function &&
        (config->connection_active_threshold_bits_per_second >
                config->minimum_download_rate_bits_per_second ||
            config->connection_active_threshold_bits_per_second >
                config->minimum_upload_rate_bits_per_second)
    ) {
        error_set(error, error_size, "connection active threshold cannot exceed either minimum shaper rate");
        return -1;
    }

    return 0;
}

int config_validate(
    const struct config *config,
    char *error,
    size_t error_size
)
{
    if (
        (config->enabled || config->adjust_download ||
            config->adjust_upload) &&
        config->interface[0] == '\0'
    ) {
        error_set(
            error,
            error_size,
            "set either option 'interface' or both options 'ul_if' and"
            " 'dl_if' when cake-adapt is enabled or rate adjustment is"
            " configured"
        );
        return -1;
    }
    if (
        validate_rate_range(
            config->adjust_download,
            config->minimum_download_rate_bits_per_second,
            config->base_download_rate_bits_per_second,
            config->maximum_download_rate_bits_per_second,
            DIRECTION_DOWNLOAD,
            error,
            error_size
        ) != 0 ||
        validate_rate_range(
            config->adjust_upload,
            config->minimum_upload_rate_bits_per_second,
            config->base_upload_rate_bits_per_second,
            config->maximum_upload_rate_bits_per_second,
            DIRECTION_UPLOAD,
            error,
            error_size
        ) != 0
    ) {
        return -1;
    }
    return validate_latency_config(config, error, error_size);
}
