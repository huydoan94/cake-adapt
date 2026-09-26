#define _GNU_SOURCE

#include "config.h"
#include "constants.h"
#include "defaults.h"
#include "error.h"
#include "helpers.h"
#include "latency.h"

#include <libubox/utils.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Current libuci headers contain inline helpers that trigger -Wsign-conversion.
 * Keep strict conversion warnings for cake-adapt while isolating that external
 * header warning.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include <uci.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct boolean_option_binding {
    const char *name;
    size_t offset;
};

struct scaled_option_binding {
    const char *name;
    size_t offset;
    uint64_t scale;
};

struct string_option_binding {
    const char *name;
    size_t offset;
    size_t destination_size;
};

#define CONFIG_OFFSET(member) offsetof(struct config, member)

static const struct boolean_option_binding boolean_options[] = {
    { OPTION_ENABLED, CONFIG_OFFSET(enabled) },
    { OPTION_ADJUST_DOWNLOAD, CONFIG_OFFSET(adjust_download) },
    { OPTION_ADJUST_UPLOAD, CONFIG_OFFSET(adjust_upload) },
    { OPTION_OUTPUT_PROCESSING_STATS, CONFIG_OFFSET(output_processing_stats) },
    { OPTION_OUTPUT_LOAD_STATS, CONFIG_OFFSET(output_load_stats) },
    { OPTION_OUTPUT_REFLECTOR_STATS, CONFIG_OFFSET(output_reflector_stats) },
    { OPTION_OUTPUT_SUMMARY_STATS, CONFIG_OFFSET(output_summary_stats) },
    { OPTION_OUTPUT_CAKE_CHANGES, CONFIG_OFFSET(output_cake_changes) },
    { OPTION_OUTPUT_CPU_STATS, CONFIG_OFFSET(output_cpu_stats) },
    { OPTION_OUTPUT_CPU_RAW_STATS, CONFIG_OFFSET(output_cpu_raw_stats) },
    { OPTION_DEBUG, CONFIG_OFFSET(debug) },
    {
        OPTION_LOG_DEBUG_TO_SYSLOG,
        CONFIG_OFFSET(log_debug_messages_to_syslog)
    },
    { OPTION_LOG_TO_FILE, CONFIG_OFFSET(log_to_file) },
    { OPTION_RANDOMIZE_REFLECTORS, CONFIG_OFFSET(randomize_reflectors) },
    { OPTION_RETAIN_REFLECTOR_STATS, CONFIG_OFFSET(retain_reflector_stats) },
    { OPTION_ENABLE_SLEEP_FUNCTION, CONFIG_OFFSET(enable_sleep_function) },
    {
        OPTION_MIN_SHAPER_RATES_ENFORCEMENT,
        CONFIG_OFFSET(minimum_shaper_rates_enforcement)
    },
    {
        OPTION_LOG_FILE_EXPORT_COMPRESS,
        CONFIG_OFFSET(log_file_export_compress)
    }
};

static const struct string_option_binding string_options[] = {
    {
        OPTION_INTERFACE,
        CONFIG_OFFSET(interface),
        sizeof(((struct config *)0)->interface)
    },
    {
        OPTION_UPLOAD_INTERFACE,
        CONFIG_OFFSET(ul_if),
        sizeof(((struct config *)0)->ul_if)
    },
    {
        OPTION_DOWNLOAD_INTERFACE,
        CONFIG_OFFSET(dl_if),
        sizeof(((struct config *)0)->dl_if)
    },
    {
        OPTION_CONFIG_FILE,
        CONFIG_OFFSET(config_file),
        sizeof(((struct config *)0)->config_file)
    },
    {
        OPTION_LOG_FILE_PATH_OVERRIDE,
        CONFIG_OFFSET(log_file_path_override),
        sizeof(((struct config *)0)->log_file_path_override)
    },
    {
        OPTION_PINGER_METHOD,
        CONFIG_OFFSET(pinger_method),
        sizeof(((struct config *)0)->pinger_method)
    },
    {
        OPTION_PING_EXTRA_ARGS,
        CONFIG_OFFSET(ping_extra_args),
        sizeof(((struct config *)0)->ping_extra_args)
    },
    {
        OPTION_PING_PREFIX_STRING,
        CONFIG_OFFSET(ping_prefix_string),
        sizeof(((struct config *)0)->ping_prefix_string)
    }
};

static const struct scaled_option_binding scaled_options[] = {
    { OPTION_LOG_FILE_MAX_TIME, CONFIG_OFFSET(log_file_max_time_minutes), 1U },
    { OPTION_LOG_FILE_MAX_SIZE, CONFIG_OFFSET(log_file_max_size_kilobytes), 1U },
    { OPTION_NO_PINGERS, CONFIG_OFFSET(no_pingers), 1U },
    {
        OPTION_REFLECTOR_PING_INTERVAL,
        CONFIG_OFFSET(reflector_ping_interval_microseconds),
        SECOND
    },
    {
        OPTION_MIN_DOWNLOAD_RATE,
        CONFIG_OFFSET(minimum_download_rate_bits_per_second),
        KILOBIT
    },
    {
        OPTION_BASE_DOWNLOAD_RATE,
        CONFIG_OFFSET(base_download_rate_bits_per_second),
        KILOBIT
    },
    {
        OPTION_MAX_DOWNLOAD_RATE,
        CONFIG_OFFSET(maximum_download_rate_bits_per_second),
        KILOBIT
    },
    {
        OPTION_MIN_UPLOAD_RATE,
        CONFIG_OFFSET(minimum_upload_rate_bits_per_second),
        KILOBIT
    },
    {
        OPTION_BASE_UPLOAD_RATE,
        CONFIG_OFFSET(base_upload_rate_bits_per_second),
        KILOBIT
    },
    {
        OPTION_MAX_UPLOAD_RATE,
        CONFIG_OFFSET(maximum_upload_rate_bits_per_second),
        KILOBIT
    },
    {
        OPTION_CONNECTION_ACTIVE_THRESHOLD,
        CONFIG_OFFSET(connection_active_threshold_bits_per_second),
        KILOBIT
    },
    {
        OPTION_CONNECTION_STALL_THRESHOLD,
        CONFIG_OFFSET(connection_stall_threshold_bits_per_second),
        KILOBIT
    },
    {
        OPTION_DOWNLOAD_AVG_ADJUST_UP,
        CONFIG_OFFSET(download_average_owd_delta_maximum_adjust_up_microseconds),
        MILLISECOND
    },
    {
        OPTION_UPLOAD_AVG_ADJUST_UP,
        CONFIG_OFFSET(upload_average_owd_delta_maximum_adjust_up_microseconds),
        MILLISECOND
    },
    {
        OPTION_DOWNLOAD_DELAY_THRESHOLD,
        CONFIG_OFFSET(download_owd_delta_delay_threshold_microseconds),
        MILLISECOND
    },
    {
        OPTION_UPLOAD_DELAY_THRESHOLD,
        CONFIG_OFFSET(upload_owd_delta_delay_threshold_microseconds),
        MILLISECOND
    },
    {
        OPTION_DOWNLOAD_AVG_ADJUST_DOWN,
        CONFIG_OFFSET(download_average_owd_delta_maximum_adjust_down_microseconds),
        MILLISECOND
    },
    {
        OPTION_UPLOAD_AVG_ADJUST_DOWN,
        CONFIG_OFFSET(upload_average_owd_delta_maximum_adjust_down_microseconds),
        MILLISECOND
    },
    {
        OPTION_SUSTAINED_IDLE_SLEEP,
        CONFIG_OFFSET(sustained_idle_sleep_threshold_microseconds),
        SECOND
    },
    {
        OPTION_LOG_FILE_BUFFER_TIMEOUT,
        CONFIG_OFFSET(log_file_buffer_timeout_microseconds),
        MILLISECOND
    },
    {
        OPTION_IRTT_SESSION_DURATION,
        CONFIG_OFFSET(irtt_session_duration_minutes),
        1U
    },
    {
        OPTION_TRAFFIC_MONITOR_INTERVAL,
        CONFIG_OFFSET(monitor_achieved_rates_interval_microseconds),
        MILLISECOND
    },
    {
        OPTION_CPU_MONITOR_INTERVAL,
        CONFIG_OFFSET(monitor_cpu_usage_interval_microseconds),
        MILLISECOND
    },
    {
        OPTION_BUFFERBLOAT_WINDOW,
        CONFIG_OFFSET(bufferbloat_detection_window),
        1U
    },
    {
        OPTION_BUFFERBLOAT_THRESHOLD,
        CONFIG_OFFSET(bufferbloat_detection_threshold),
        1U
    },
    {
        OPTION_ALPHA_BASELINE_INCREASE,
        CONFIG_OFFSET(alpha_baseline_increase_per_million),
        MILLION
    },
    {
        OPTION_ALPHA_BASELINE_DECREASE,
        CONFIG_OFFSET(alpha_baseline_decrease_per_million),
        MILLION
    },
    {
        OPTION_ALPHA_DELTA_EWMA,
        CONFIG_OFFSET(alpha_delta_ewma_per_million),
        MILLION
    },
    {
        OPTION_RATE_MIN_DOWN_BUFFERBLOAT,
        CONFIG_OFFSET(shaper_rate_minimum_adjust_down_bufferbloat_per_million),
        MILLION
    },
    {
        OPTION_RATE_MAX_DOWN_BUFFERBLOAT,
        CONFIG_OFFSET(shaper_rate_maximum_adjust_down_bufferbloat_per_million),
        MILLION
    },
    {
        OPTION_RATE_MIN_UP_HIGH_LOAD,
        CONFIG_OFFSET(shaper_rate_minimum_adjust_up_load_high_per_million),
        MILLION
    },
    {
        OPTION_RATE_MAX_UP_HIGH_LOAD,
        CONFIG_OFFSET(shaper_rate_maximum_adjust_up_load_high_per_million),
        MILLION
    },
    {
        OPTION_RATE_DOWN_LOW_LOAD,
        CONFIG_OFFSET(shaper_rate_adjust_down_load_low_per_million),
        MILLION
    },
    {
        OPTION_RATE_UP_LOW_LOAD,
        CONFIG_OFFSET(shaper_rate_adjust_up_load_low_per_million),
        MILLION
    },
    {
        OPTION_HIGH_LOAD_THRESHOLD,
        CONFIG_OFFSET(high_load_threshold_per_million),
        MILLION
    },
    {
        OPTION_BUFFERBLOAT_REFRACTORY,
        CONFIG_OFFSET(bufferbloat_refractory_period_microseconds),
        MILLISECOND
    },
    {
        OPTION_DECAY_REFRACTORY,
        CONFIG_OFFSET(decay_refractory_period_microseconds),
        MILLISECOND
    },
    {
        OPTION_REFLECTOR_HEALTH_INTERVAL,
        CONFIG_OFFSET(reflector_health_check_interval_microseconds),
        SECOND
    },
    {
        OPTION_REFLECTOR_RESPONSE_DEADLINE,
        CONFIG_OFFSET(reflector_response_deadline_microseconds),
        SECOND
    },
    {
        OPTION_REFLECTOR_MISBEHAVING_WINDOW,
        CONFIG_OFFSET(reflector_misbehaving_detection_window),
        1U
    },
    {
        OPTION_REFLECTOR_MISBEHAVING_THRESHOLD,
        CONFIG_OFFSET(reflector_misbehaving_detection_threshold),
        1U
    },
    {
        OPTION_REFLECTOR_REPLACEMENT_INTERVAL,
        CONFIG_OFFSET(reflector_replacement_interval_minutes),
        1U
    },
    {
        OPTION_REFLECTOR_COMPARISON_INTERVAL,
        CONFIG_OFFSET(reflector_comparison_interval_minutes),
        1U
    },
    {
        OPTION_REFLECTOR_BASELINE_DELTA,
        CONFIG_OFFSET(reflector_sum_owd_baselines_delta_threshold_microseconds),
        MILLISECOND
    },
    {
        OPTION_REFLECTOR_EWMA_DELTA,
        CONFIG_OFFSET(reflector_owd_delta_ewma_delta_threshold_microseconds),
        MILLISECOND
    },
    {
        OPTION_STALL_DETECTION_THRESHOLD,
        CONFIG_OFFSET(stall_detection_threshold),
        1U
    },
    {
        OPTION_GLOBAL_PING_TIMEOUT,
        CONFIG_OFFSET(global_ping_response_timeout_microseconds),
        SECOND
    },
    {
        OPTION_INTERFACE_UP_INTERVAL,
        CONFIG_OFFSET(interface_up_check_interval_microseconds),
        SECOND
    }
};

size_t config_option_count(void)
{
    return ARRAY_SIZE(boolean_options) +
        ARRAY_SIZE(string_options) +
        ARRAY_SIZE(scaled_options);
}

const char *config_option_name(size_t index)
{
    if (index < ARRAY_SIZE(boolean_options)) {
        return boolean_options[index].name;
    }
    index -= ARRAY_SIZE(boolean_options);
    if (index < ARRAY_SIZE(string_options)) {
        return string_options[index].name;
    }
    index -= ARRAY_SIZE(string_options);
    if (index < ARRAY_SIZE(scaled_options)) {
        return scaled_options[index].name;
    }
    return NULL;
}

static int copy_reflector(
    struct config *config,
    const char *reflector,
    char *error,
    size_t error_size
);

static int copy_option(
    char *destination,
    size_t destination_size,
    const char *value,
    const char *option_name,
    char *error,
    size_t error_size
)
{
    size_t length = strlen(value);

    if (length >= destination_size) {
        error_set(
            error,
            error_size,
            "option '%s' is too long",
            option_name
        );
        return -1;
    }

    memcpy(destination, value, length + 1U);
    return 0;
}

static int resolve_interfaces(
    struct config *config,
    char *error,
    size_t error_size
)
{
    bool upload_configured = config->ul_if[0] != '\0';
    bool download_configured = config->dl_if[0] != '\0';

    config->interface_overridden = false;
    if (upload_configured != download_configured) {
        error_set(
            error,
            error_size,
            "options 'ul_if' and 'dl_if' must be configured together"
        );
        return -1;
    }
    /* The mismatch check above guarantees dl_if is configured here too. */
    if (upload_configured) {
        config->interface_overridden = config->interface[0] != '\0';
        memcpy(
            config->interface,
            config->ul_if,
            sizeof(config->interface)
        );
        memcpy(
            config->ingress_interface,
            config->dl_if,
            sizeof(config->ingress_interface)
        );
        return 0;
    }

    config->ingress_interface[0] = '\0';
    if (config->interface[0] == '\0') {
        return 0;
    }

    /* SQM names its ingress IFB "ifb4<interface>", truncated to IFNAMSIZ. */
    (void)snprintf(
        config->ingress_interface,
        sizeof(config->ingress_interface),
        IFB_PREFIX "%.*s",
        (int)(sizeof(config->ingress_interface) - sizeof(IFB_PREFIX)),
        config->interface
    );
    return 0;
}

static int parse_boolean(
    const char *value,
    bool *result
)
{
    if (
        strcmp(value, BOOLEAN_TRUE_NUMERIC) == 0 ||
        strcasecmp(value, BOOLEAN_TRUE) == 0 ||
        strcasecmp(value, BOOLEAN_YES) == 0 ||
        strcasecmp(value, BOOLEAN_ON) == 0
    ) {
        *result = true;
        return 0;
    }

    if (
        strcmp(value, BOOLEAN_FALSE_NUMERIC) == 0 ||
        strcasecmp(value, BOOLEAN_FALSE) == 0 ||
        strcasecmp(value, BOOLEAN_NO) == 0 ||
        strcasecmp(value, BOOLEAN_OFF) == 0
    ) {
        *result = false;
        return 0;
    }

    return -1;
}

static int lookup_string_option(
    struct uci_context *context,
    struct uci_section *section,
    const char *name,
    const char **value,
    char *error,
    size_t error_size
)
{
    struct uci_option *option = uci_lookup_option(context, section, name);

    *value = NULL;
    if (option == NULL) {
        return 0;
    }
    if (option->type != UCI_TYPE_STRING) {
        error_set(error, error_size, "option '%s' must be a scalar UCI option, not a list", name);
        return -1;
    }
    *value = option->v.string;
    return 0;
}

static int load_boolean_options(
    struct uci_context *context,
    struct uci_section *section,
    struct config *config,
    char *error,
    size_t error_size
)
{
    size_t index;

    for (index = 0U; index < ARRAY_SIZE(boolean_options); index++) {
        const char *value;
        bool *destination = (bool *)(
            (char *)config + boolean_options[index].offset
        );

        if (
            lookup_string_option(
                context,
                section,
                boolean_options[index].name,
                &value,
                error,
                error_size
            ) != 0
        ) {
            return -1;
        }

        if (
            value != NULL &&
            parse_boolean(value, destination) != 0
        ) {
            error_set(
                error,
                error_size,
                "option '%s' is not a boolean",
                boolean_options[index].name
            );
            return -1;
        }
    }
    return 0;
}

static int parse_scaled_decimal(
    const char *value,
    uint64_t scale,
    uint64_t *result,
    const char *option_name,
    char *error,
    size_t error_size
)
{
    const char *character = value;
    uint64_t scaled_value = 0U;
    uint64_t fractional_place;
    size_t integer_digits;

    if (value[0] == '\0') {
        error_set(error, error_size, "option '%s' is empty", option_name);
        return -1;
    }

    integer_digits = strspn(value, DECIMAL_DIGITS);
    character = value + integer_digits;
    if (
        integer_digits != 0U &&
        !parse_unsigned(value, character, &scaled_value)
    ) {
        error_set(error, error_size, "option '%s' is too large", option_name);
        return -1;
    }
    if (
        character == value ||
        (scaled_value > UINT64_MAX / scale)
    ) {
        error_set(
            error,
            error_size,
            "option '%s' is not a non-negative decimal",
            option_name
        );
        return -1;
    }
    scaled_value *= scale;

    if (*character == '\0') {
        *result = scaled_value;
        return 0;
    }
    if (
        *character != '.' ||
        character[1] == '\0'
    ) {
        error_set(
            error,
            error_size,
            "option '%s' is not a non-negative decimal",
            option_name
        );
        return -1;
    }

    fractional_place = scale;
    for (character++; *character != '\0'; character++) {
        unsigned int digit;

        if (
            *character < '0' ||
            *character > '9'
        ) {
            error_set(
                error,
                error_size,
                "option '%s' is not a non-negative decimal",
                option_name
            );
            return -1;
        }
        digit = (unsigned int)(*character - '0');
        if (fractional_place > 1U) {
            uint64_t contribution;

            fractional_place /= 10U;
            contribution = (uint64_t)digit * fractional_place;
            if (scaled_value > UINT64_MAX - contribution) {
                error_set(
                    error,
                    error_size,
                    "option '%s' is too large",
                    option_name
                );
                return -1;
            }
            scaled_value += contribution;
        } else if (digit != 0U) {
            error_set(
                error,
                error_size,
                "option '%s' has more precision than cake-adapt stores",
                option_name
            );
            return -1;
        }
    }

    *result = scaled_value;
    return 0;
}

static int load_scaled_options(
    struct uci_context *context,
    struct uci_section *section,
    struct config *config,
    char *error,
    size_t error_size
)
{
    size_t index;

    for (index = 0U; index < ARRAY_SIZE(scaled_options); index++) {
        const char *value;
        uint64_t *destination = (uint64_t *)(
            (char *)config + scaled_options[index].offset
        );

        if (
            lookup_string_option(
                context,
                section,
                scaled_options[index].name,
                &value,
                error,
                error_size
            ) != 0
        ) {
            return -1;
        }

        if (
            value != NULL &&
            parse_scaled_decimal(
                value,
                scaled_options[index].scale,
                destination,
                scaled_options[index].name,
                error,
                error_size
            ) != 0
        ) {
            return -1;
        }
    }
    return 0;
}

static int load_string_options(
    struct uci_context *context,
    struct uci_section *section,
    struct config *config,
    char *error,
    size_t error_size
)
{
    size_t index;

    for (index = 0U; index < ARRAY_SIZE(string_options); index++) {
        const char *value;
        char *destination = (char *)config + string_options[index].offset;

        if (
            lookup_string_option(
                context,
                section,
                string_options[index].name,
                &value,
                error,
                error_size
            ) != 0
        ) {
            return -1;
        }

        if (
            value != NULL &&
            copy_option(
                destination,
                string_options[index].destination_size,
                value,
                string_options[index].name,
                error,
                error_size
            ) != 0
        ) {
            return -1;
        }
    }
    return 0;
}


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
    if (strcmp(config->pinger_method, PINGER_METHOD_FPING) != 0) {
        error_set(error, error_size, "option 'pinger_method' must be 'fping'; no other pinger is supported");
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

static int copy_reflector(
    struct config *config,
    const char *reflector,
    char *error,
    size_t error_size
)
{
    uint64_t index = config->reflector_count;

    if (index >= CONFIG_MAX_REFLECTORS) {
        error_set(
            error,
            error_size,
            "option 'reflectors' contains more than %u entries",
            CONFIG_MAX_REFLECTORS
        );
        return -1;
    }
    if (
        copy_option(
            config->reflectors[index],
            sizeof(config->reflectors[index]),
            reflector,
            OPTION_REFLECTORS,
            error,
            error_size
        ) != 0
    ) {
        return -1;
    }
    config->reflector_count++;
    return 0;
}

static int load_reflectors(
    struct uci_context *context,
    struct uci_section *section,
    struct config *config,
    char *error,
    size_t error_size
)
{
    struct uci_option *option = uci_lookup_option(
        context,
        section,
        OPTION_REFLECTORS
    );
    struct uci_element *element;

    if (option == NULL) {
        return 0;
    }
    if (option->type != UCI_TYPE_LIST) {
        error_set(error, error_size, "option 'reflectors' must be a UCI list");
        return -1;
    }

    config->reflector_count = 0U;
    uci_foreach_element(&option->v.list, element) {
        if (
            copy_reflector(
                config,
                element->name,
                error,
                error_size
            ) != 0
        ) {
            return -1;
        }
    }
    return 0;
}

static int validate_config(
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

static int load_section(
    struct uci_context *context,
    struct uci_section *section,
    struct config *config,
    char *error,
    size_t error_size
)
{
    bool reflectors_configured = uci_lookup_option(
        context,
        section,
        OPTION_REFLECTORS
    ) != NULL;
    bool no_pingers_configured = uci_lookup_option(
        context,
        section,
        OPTION_NO_PINGERS
    ) != NULL;
    /* Legacy single-target input is needed only while loading configuration. */
    char latency_target[CONFIG_REFLECTOR_SIZE] = { 0 };
    const char *latency_target_value;

    if (
        lookup_string_option(
            context,
            section,
            OPTION_LATENCY_TARGET,
            &latency_target_value,
            error,
            error_size
        ) != 0 ||
        (latency_target_value != NULL &&
            copy_option(
                latency_target,
                sizeof(latency_target),
                latency_target_value,
                OPTION_LATENCY_TARGET,
                error,
                error_size
            ) != 0)
    ) {
        return -1;
    }

    if (
        load_boolean_options(
            context,
            section,
            config,
            error,
            error_size
        ) != 0 ||
        load_string_options(
            context,
            section,
            config,
            error,
            error_size
        ) != 0 ||
        load_scaled_options(
            context,
            section,
            config,
            error,
            error_size
        ) != 0 ||
        load_reflectors(
            context,
            section,
            config,
            error,
            error_size
        ) != 0
    ) {
        return -1;
    }

    if (resolve_interfaces(config, error, error_size) != 0) {
        return -1;
    }

    if (
        !reflectors_configured &&
        latency_target[0] != '\0'
    ) {
        config->reflector_count = 0U;
        if (
            copy_reflector(
                config,
                latency_target,
                error,
                error_size
            ) != 0
        ) {
            return -1;
        }
        if (!no_pingers_configured) {
            config->no_pingers = 1U;
        }
    }

    return validate_config(config, error, error_size);
}

int config_load(
    struct config *config,
    const char *config_directory,
    const char *section_name,
    char *error,
    size_t error_size
)
{
    struct uci_context *context;
    struct uci_package *package = NULL;
    struct uci_section *section;
    int result = -1;

    if (
        section_name == NULL ||
        section_name[0] == '\0'
    ) {
        error_set(error, error_size, "UCI section name is empty");
        return -1;
    }

    defaults_apply(config);

    context = uci_alloc_context();
    if (context == NULL) {
        error_set(error, error_size, "could not allocate a UCI context");
        return -1;
    }

    if (config_directory != NULL) {
        if (uci_set_confdir(context, config_directory) != UCI_OK) {
            error_set(
                error,
                error_size,
                "could not use UCI configuration directory '%s'",
                config_directory
            );
            goto done;
        }
    }

    if (uci_load(context, UCI_PACKAGE, &package) != UCI_OK) {
        char *uci_error = NULL;

        uci_get_errorstr(context, &uci_error, UCI_PACKAGE);
        error_set(
            error,
            error_size,
            "%s",
            uci_error != NULL ? uci_error : "could not load UCI configuration"
        );
        free(uci_error);
        goto done;
    }

    section = uci_lookup_section(context, package, section_name);
    if (
        section == NULL ||
        strcmp(section->type, UCI_SECTION_TYPE) != 0
    ) {
        error_set(
            error,
            error_size,
            "missing config cake_adapt '%s' section",
            section_name
        );
        goto done;
    }

    result = load_section(
        context,
        section,
        config,
        error,
        error_size
    );

done:
    /* libuci owns and releases every package loaded into this context. */
    uci_free_context(context);
    return result;
}
