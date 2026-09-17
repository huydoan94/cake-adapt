#define _GNU_SOURCE

#include "config.h"
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

#define UCI_PACKAGE "cake-adapt"
#define UCI_SECTION "main"
#define UCI_SECTION_TYPE "cake_adapt"
#define IFB_PREFIX "ifb4"

struct boolean_option_binding {
    const char *name;
    bool *destination;
};

struct scaled_option_binding {
    const char *name;
    uint64_t *destination;
    uint64_t scale;
};

struct string_option_binding {
    const char *name;
    char *destination;
    size_t destination_size;
};

static int copy_option(
    char *destination,
    size_t destination_size,
    const char *value,
    const char *option_name,
    char *error,
    size_t error_size
)
{
    int result;

    result = snprintf(destination, destination_size, "%s", value);
    if (result < 0 || (size_t)result >= destination_size) {
        error_set(
            error,
            error_size,
            "option '%s' is too long",
            option_name
        );
        return -1;
    }

    return 0;
}

static void derive_ingress_interface(struct sqm_mon_config *config)
{
    config->ingress_interface[0] = '\0';
    if (config->interface[0] == '\0') {
        return;
    }

    /* SQM names its ingress IFB "ifb4<interface>", truncated to IFNAMSIZ. */
    (void)snprintf(
        config->ingress_interface,
        sizeof(config->ingress_interface),
        IFB_PREFIX "%.*s",
        (int)(sizeof(config->ingress_interface) - sizeof(IFB_PREFIX)),
        config->interface
    );
}

static int parse_boolean(
    const char *value,
    bool *result
)
{
    if (strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0) {
        *result = true;
        return 0;
    }

    if (strcmp(value, "0") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0) {
        *result = false;
        return 0;
    }

    return -1;
}

static int load_boolean_options(
    struct uci_context *context,
    struct uci_section *section,
    const struct boolean_option_binding *options,
    size_t option_count,
    char *error,
    size_t error_size
)
{
    size_t index;

    for (index = 0U; index < option_count; index++) {
        const char *value = uci_lookup_option_string(
            context,
            section,
            options[index].name
        );

        if (value != NULL &&
            parse_boolean(value, options[index].destination) != 0) {
            error_set(
                error,
                error_size,
                "option '%s' is not a boolean",
                options[index].name
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

    if (value[0] == '\0' || scale == 0U) {
        error_set(error, error_size, "option '%s' is empty", option_name);
        return -1;
    }

    integer_digits = strspn(value, "0123456789");
    character = value + integer_digits;
    if (integer_digits != 0U && !parse_unsigned(value, character, &scaled_value)) {
        error_set(error, error_size, "option '%s' is too large", option_name);
        return -1;
    }
    if (character == value ||
        (scaled_value > UINT64_MAX / scale)) {
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
    if (*character != '.' || character[1] == '\0') {
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

        if (*character < '0' || *character > '9') {
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
    const struct scaled_option_binding *options,
    size_t option_count,
    char *error,
    size_t error_size
)
{
    size_t index;

    for (index = 0U; index < option_count; index++) {
        const char *value = uci_lookup_option_string(
            context,
            section,
            options[index].name
        );

        if (value != NULL &&
            parse_scaled_decimal(
                value,
                options[index].scale,
                options[index].destination,
                options[index].name,
                error,
                error_size
            ) != 0) {
            return -1;
        }
    }
    return 0;
}

static int load_string_options(
    struct uci_context *context,
    struct uci_section *section,
    const struct string_option_binding *options,
    size_t option_count,
    char *error,
    size_t error_size
)
{
    size_t index;

    for (index = 0U; index < option_count; index++) {
        const char *value = uci_lookup_option_string(
            context,
            section,
            options[index].name
        );

        if (value != NULL &&
            copy_option(
                options[index].destination,
                options[index].destination_size,
                value,
                options[index].name,
                error,
                error_size
            ) != 0) {
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
    if (!adjust && minimum == 0U && base == 0U && maximum == 0U) {
        return 0;
    }
    if (minimum == 0U || base == 0U || maximum == 0U) {
        error_set(
            error,
            error_size,
            "%s min/base/max rates are required together",
            direction
        );
        return -1;
    }
    if (minimum > base || base > maximum) {
        error_set(
            error,
            error_size,
            "%s rates must satisfy minimum <= base <= maximum",
            direction
        );
        return -1;
    }
    return 0;
}

static int validate_reflectors(
    const struct sqm_mon_config *config,
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
    if (config->no_pingers == 0U ||
        config->no_pingers > config->reflector_count) {
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
    const struct sqm_mon_config *config,
    char *error,
    size_t error_size
)
{
    if (!config->enabled && !config->adjust_download && !config->adjust_upload) {
        return 0;
    }
    if (strcmp(config->pinger_method, "fping") != 0) {
        error_set(error, error_size, "option 'pinger_method' must be 'fping'; no other pinger is supported");
        return -1;
    }
    if (config->no_pingers == 0U || config->no_pingers > CONFIG_MAX_REFLECTORS) {
        error_set(error, error_size, "option 'no_pingers' must be between 1 and %u", CONFIG_MAX_REFLECTORS);
        return -1;
    }
    if (validate_reflectors(config, error, error_size) != 0) {
        return -1;
    }
    if (config->reflector_ping_interval_microseconds /
            config->no_pingers < 1000U) {
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
    if (config->monitor_achieved_rates_interval_microseconds % 1000U != 0U) {
        error_set(
            error,
            error_size,
            "option 'monitor_achieved_rates_interval_ms' must be a whole"
            " number of milliseconds"
        );
        return -1;
    }
    if (config->monitor_achieved_rates_interval_microseconds / 1000U >
        UINT_MAX) {
        error_set(
            error,
            error_size,
            "option 'monitor_achieved_rates_interval_ms' is too large"
        );
        return -1;
    }
    if (config->bufferbloat_detection_window == 0U ||
        config->bufferbloat_detection_window > UINT_MAX) {
        error_set(
            error,
            error_size,
            "option 'bufferbloat_detection_window' must be between 1 and %u",
            UINT_MAX
        );
        return -1;
    }
    if (config->bufferbloat_detection_threshold >
        config->bufferbloat_detection_window) {
        error_set(
            error,
            error_size,
            "option 'bufferbloat_detection_thr' cannot be greater than"
            " 'bufferbloat_detection_window'"
        );
        return -1;
    }
    if (config->alpha_baseline_increase_per_million > 1000000U ||
        config->alpha_baseline_decrease_per_million > 1000000U ||
        config->alpha_delta_ewma_per_million > 1000000U) {
        error_set(
            error,
            error_size,
            "alpha options must be between 0 and 1"
        );
        return -1;
    }
    if (config->reflector_health_check_interval_microseconds == 0U ||
        config->reflector_response_deadline_microseconds == 0U) {
        error_set(
            error,
            error_size,
            "reflector health interval and response deadline must be positive"
        );
        return -1;
    }
    if (config->reflector_health_check_interval_microseconds % 1000U != 0U ||
        config->reflector_health_check_interval_microseconds / 1000U >
            UINT_MAX) {
        error_set(
            error,
            error_size,
            "option 'reflector_health_check_interval_s' must be a whole"
            " number of milliseconds no greater than %u",
            UINT_MAX
        );
        return -1;
    }
    if (config->reflector_misbehaving_detection_window == 0U ||
        config->reflector_misbehaving_detection_window > SIZE_MAX ||
        config->reflector_misbehaving_detection_threshold == 0U ||
        config->reflector_misbehaving_detection_threshold >
            config->reflector_misbehaving_detection_window) {
        error_set(
            error,
            error_size,
            "reflector offence threshold must be between 1 and its window"
        );
        return -1;
    }
    if (config->reflector_replacement_interval_minutes >
            UINT64_MAX / 60000000U ||
        config->reflector_comparison_interval_minutes >
            UINT64_MAX / 60000000U) {
        error_set(
            error,
            error_size,
            "reflector replacement and comparison intervals are too large"
        );
        return -1;
    }
    if (config->stall_detection_threshold == 0U ||
        config->stall_detection_threshold >
            UINT64_MAX /
                (config->reflector_ping_interval_microseconds /
                    config->no_pingers) ||
        config->global_ping_response_timeout_microseconds == 0U ||
        config->interface_up_check_interval_microseconds == 0U) {
        error_set(
            error,
            error_size,
            "stall, global ping timeout and interface retry settings must be"
            " positive and representable"
        );
        return -1;
    }
    if ((config->output_cpu_stats || config->output_cpu_raw_stats) &&
        (config->monitor_cpu_usage_interval_microseconds == 0U ||
            config->monitor_cpu_usage_interval_microseconds % 1000U != 0U ||
            config->monitor_cpu_usage_interval_microseconds / 1000U > UINT_MAX)) {
        error_set(
            error,
            error_size,
            "CPU monitoring interval must be a positive whole number of milliseconds no greater than %u",
            UINT_MAX
        );
        return -1;
    }
    if (config->log_file_max_time_minutes > UINT64_MAX / 60000000U ||
        config->log_file_max_size_kilobytes > UINT64_MAX / 1024U ||
        config->log_file_buffer_timeout_microseconds / 1000U > UINT_MAX ||
        config->reflector_ping_interval_microseconds > UINT64_MAX / 2U) {
        error_set(error, error_size, "logging or pinger intervals are too large");
        return -1;
    }
    if (config->enable_sleep_function &&
        (config->connection_active_threshold_bits_per_second >
                config->minimum_download_rate_bits_per_second ||
            config->connection_active_threshold_bits_per_second >
                config->minimum_upload_rate_bits_per_second)) {
        error_set(error, error_size, "connection active threshold cannot exceed either minimum shaper rate");
        return -1;
    }

    return 0;
}

static int copy_reflector(
    struct sqm_mon_config *config,
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
    if (copy_option(
            config->reflectors[index],
            sizeof(config->reflectors[index]),
            reflector,
            "reflectors",
            error,
            error_size
        ) != 0) {
        return -1;
    }
    config->reflector_count++;
    return 0;
}

static int load_reflectors(
    struct uci_context *context,
    struct uci_section *section,
    struct sqm_mon_config *config,
    char *error,
    size_t error_size
)
{
    struct uci_option *option = uci_lookup_option(
        context,
        section,
        "reflectors"
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
        if (copy_reflector(
                config,
                element->name,
                error,
                error_size
            ) != 0) {
            return -1;
        }
    }
    return 0;
}

static int load_section(
    struct uci_context *context,
    struct uci_section *section,
    struct sqm_mon_config *config,
    char *error,
    size_t error_size
)
{
    bool reflectors_configured = uci_lookup_option(
        context,
        section,
        "reflectors"
    ) != NULL;
    bool no_pingers_configured = uci_lookup_option(
        context,
        section,
        "no_pingers"
    ) != NULL;
    const struct boolean_option_binding boolean_options[] = {
        { "enabled", &config->enabled },
        { "adjust_dl_shaper_rate", &config->adjust_download },
        { "adjust_ul_shaper_rate", &config->adjust_upload },
        { "output_processing_stats", &config->output_processing_stats },
        { "output_load_stats", &config->output_load_stats },
        { "output_reflector_stats", &config->output_reflector_stats },
        { "output_summary_stats", &config->output_summary_stats },
        { "output_cake_changes", &config->output_cake_changes },
        { "output_cpu_stats", &config->output_cpu_stats },
        { "output_cpu_raw_stats", &config->output_cpu_raw_stats },
        { "debug", &config->debug },
        {
            "log_DEBUG_messages_to_syslog",
            &config->log_debug_messages_to_syslog
        },
        { "log_to_file", &config->log_to_file },
        { "randomize_reflectors", &config->randomize_reflectors },
        { "retain_reflector_stats", &config->retain_reflector_stats },
        { "enable_sleep_function", &config->enable_sleep_function },
        {
            "min_shaper_rates_enforcement",
            &config->minimum_shaper_rates_enforcement
        },
        {
            "log_file_export_compress",
            &config->log_file_export_compress
        }
    };
    const struct string_option_binding string_options[] = {
        {
            "interface",
            config->interface,
            sizeof(config->interface)
        },
        {
            "latency_target",
            config->latency_target,
            sizeof(config->latency_target)
        },
        {
            "log_file_path_override",
            config->log_file_path_override,
            sizeof(config->log_file_path_override)
        },
        {
            "pinger_method",
            config->pinger_method,
            sizeof(config->pinger_method)
        },
        {
            "ping_extra_args",
            config->ping_extra_args,
            sizeof(config->ping_extra_args)
        },
        {
            "ping_prefix_string",
            config->ping_prefix_string,
            sizeof(config->ping_prefix_string)
        }
    };
    const struct scaled_option_binding scaled_options[] = {
        {
            "log_file_max_time_mins",
            &config->log_file_max_time_minutes,
            1U
        },
        {
            "log_file_max_size_KB",
            &config->log_file_max_size_kilobytes,
            1U
        },
        { "no_pingers", &config->no_pingers, 1U },
        {
            "reflector_ping_interval_s",
            &config->reflector_ping_interval_microseconds,
            1000000U
        },
        {
            "min_dl_shaper_rate_kbps",
            &config->minimum_download_rate_bits_per_second,
            1000U
        },
        {
            "base_dl_shaper_rate_kbps",
            &config->base_download_rate_bits_per_second,
            1000U
        },
        {
            "max_dl_shaper_rate_kbps",
            &config->maximum_download_rate_bits_per_second,
            1000U
        },
        {
            "min_ul_shaper_rate_kbps",
            &config->minimum_upload_rate_bits_per_second,
            1000U
        },
        {
            "base_ul_shaper_rate_kbps",
            &config->base_upload_rate_bits_per_second,
            1000U
        },
        {
            "max_ul_shaper_rate_kbps",
            &config->maximum_upload_rate_bits_per_second,
            1000U
        },
        {
            "connection_active_thr_kbps",
            &config->connection_active_threshold_bits_per_second,
            1000U
        },
        {
            "connection_stall_thr_kbps",
            &config->connection_stall_threshold_bits_per_second,
            1000U
        },
        {
            "dl_avg_owd_delta_max_adjust_up_thr_ms",
            &config->download_average_owd_delta_maximum_adjust_up_microseconds,
            1000U
        },
        {
            "ul_avg_owd_delta_max_adjust_up_thr_ms",
            &config->upload_average_owd_delta_maximum_adjust_up_microseconds,
            1000U
        },
        {
            "dl_owd_delta_delay_thr_ms",
            &config->download_owd_delta_delay_threshold_microseconds,
            1000U
        },
        {
            "ul_owd_delta_delay_thr_ms",
            &config->upload_owd_delta_delay_threshold_microseconds,
            1000U
        },
        {
            "dl_avg_owd_delta_max_adjust_down_thr_ms",
            &config->download_average_owd_delta_maximum_adjust_down_microseconds,
            1000U
        },
        {
            "ul_avg_owd_delta_max_adjust_down_thr_ms",
            &config->upload_average_owd_delta_maximum_adjust_down_microseconds,
            1000U
        },
        {
            "sustained_idle_sleep_thr_s",
            &config->sustained_idle_sleep_threshold_microseconds,
            1000000U
        },
        {
            "log_file_buffer_timeout_ms",
            &config->log_file_buffer_timeout_microseconds,
            1000U
        },
        {
            "irtt_session_duration_m",
            &config->irtt_session_duration_minutes,
            1U
        },
        {
            "monitor_achieved_rates_interval_ms",
            &config->monitor_achieved_rates_interval_microseconds,
            1000U
        },
        {
            "monitor_cpu_usage_interval_ms",
            &config->monitor_cpu_usage_interval_microseconds,
            1000U
        },
        {
            "bufferbloat_detection_window",
            &config->bufferbloat_detection_window,
            1U
        },
        {
            "bufferbloat_detection_thr",
            &config->bufferbloat_detection_threshold,
            1U
        },
        {
            "alpha_baseline_increase",
            &config->alpha_baseline_increase_per_million,
            1000000U
        },
        {
            "alpha_baseline_decrease",
            &config->alpha_baseline_decrease_per_million,
            1000000U
        },
        {
            "alpha_delta_ewma",
            &config->alpha_delta_ewma_per_million,
            1000000U
        },
        {
            "shaper_rate_min_adjust_down_bufferbloat",
            &config->shaper_rate_minimum_adjust_down_bufferbloat_per_million,
            1000000U
        },
        {
            "shaper_rate_max_adjust_down_bufferbloat",
            &config->shaper_rate_maximum_adjust_down_bufferbloat_per_million,
            1000000U
        },
        {
            "shaper_rate_min_adjust_up_load_high",
            &config->shaper_rate_minimum_adjust_up_load_high_per_million,
            1000000U
        },
        {
            "shaper_rate_max_adjust_up_load_high",
            &config->shaper_rate_maximum_adjust_up_load_high_per_million,
            1000000U
        },
        {
            "shaper_rate_adjust_down_load_low",
            &config->shaper_rate_adjust_down_load_low_per_million,
            1000000U
        },
        {
            "shaper_rate_adjust_up_load_low",
            &config->shaper_rate_adjust_up_load_low_per_million,
            1000000U
        },
        {
            "high_load_thr",
            &config->high_load_threshold_per_million,
            1000000U
        },
        {
            "bufferbloat_refractory_period_ms",
            &config->bufferbloat_refractory_period_microseconds,
            1000U
        },
        {
            "decay_refractory_period_ms",
            &config->decay_refractory_period_microseconds,
            1000U
        },
        {
            "reflector_health_check_interval_s",
            &config->reflector_health_check_interval_microseconds,
            1000000U
        },
        {
            "reflector_response_deadline_s",
            &config->reflector_response_deadline_microseconds,
            1000000U
        },
        {
            "reflector_misbehaving_detection_window",
            &config->reflector_misbehaving_detection_window,
            1U
        },
        {
            "reflector_misbehaving_detection_thr",
            &config->reflector_misbehaving_detection_threshold,
            1U
        },
        {
            "reflector_replacement_interval_mins",
            &config->reflector_replacement_interval_minutes,
            1U
        },
        {
            "reflector_comparison_interval_mins",
            &config->reflector_comparison_interval_minutes,
            1U
        },
        {
            "reflector_sum_owd_baselines_delta_thr_ms",
            &config->reflector_sum_owd_baselines_delta_threshold_microseconds,
            1000U
        },
        {
            "reflector_owd_delta_ewma_delta_thr_ms",
            &config->reflector_owd_delta_ewma_delta_threshold_microseconds,
            1000U
        },
        {
            "stall_detection_thr",
            &config->stall_detection_threshold,
            1U
        },
        {
            "global_ping_response_timeout_s",
            &config->global_ping_response_timeout_microseconds,
            1000000U
        },
        {
            "if_up_check_interval_s",
            &config->interface_up_check_interval_microseconds,
            1000000U
        }
    };

    if (load_boolean_options(
            context,
            section,
            boolean_options,
            ARRAY_SIZE(boolean_options),
            error,
            error_size
        ) != 0 ||
        load_string_options(
            context,
            section,
            string_options,
            ARRAY_SIZE(string_options),
            error,
            error_size
        ) != 0 ||
        load_scaled_options(
            context,
            section,
            scaled_options,
            ARRAY_SIZE(scaled_options),
            error,
            error_size
        ) != 0 ||
        load_reflectors(
            context,
            section,
            config,
            error,
            error_size
        ) != 0) {
        return -1;
    }

    derive_ingress_interface(config);

    if (!reflectors_configured && config->latency_target[0] != '\0') {
        config->reflector_count = 0U;
        if (copy_reflector(
                config,
                config->latency_target,
                error,
                error_size
            ) != 0) {
            return -1;
        }
        if (!no_pingers_configured) {
            config->no_pingers = 1U;
        }
    } else if (config->latency_target[0] == '\0' &&
        config->reflector_count > 0U &&
        copy_option(
            config->latency_target,
            sizeof(config->latency_target),
            config->reflectors[0],
            "reflectors",
            error,
            error_size
        ) != 0) {
        return -1;
    }

    if ((config->enabled || config->adjust_download ||
            config->adjust_upload) &&
        config->interface[0] == '\0') {
        error_set(
            error,
            error_size,
            "option 'interface' is required when cake-adapt is enabled"
            " or rate adjustment is configured"
        );
        return -1;
    }
    if (validate_rate_range(
            config->adjust_download,
            config->minimum_download_rate_bits_per_second,
            config->base_download_rate_bits_per_second,
            config->maximum_download_rate_bits_per_second,
            "download",
            error,
            error_size
        ) != 0 ||
        validate_rate_range(
            config->adjust_upload,
            config->minimum_upload_rate_bits_per_second,
            config->base_upload_rate_bits_per_second,
            config->maximum_upload_rate_bits_per_second,
            "upload",
            error,
            error_size
        ) != 0) {
        return -1;
    }
    if (validate_latency_config(config, error, error_size) != 0) {
        return -1;
    }

    return 0;
}

int config_load(
    struct sqm_mon_config *config,
    const char *config_directory,
    char *error,
    size_t error_size
)
{
    struct uci_context *context;
    struct uci_package *package = NULL;
    struct uci_section *section;
    int result = -1;

    if (config == NULL) {
        error_set(error, error_size, "configuration destination is null");
        return -1;
    }

    *config = (struct sqm_mon_config) {
        /* Adjustment remains opt-in even though cake-autorate defaults it on. */
        .adjust_download = false,
        .adjust_upload = false,
        .debug = true,
        .log_to_file = true,
        .randomize_reflectors = true,
        .retain_reflector_stats = true,
        .enable_sleep_function = true,
        .log_file_export_compress = true,
        .pinger_method = "fping",
        .log_file_max_time_minutes = 10U,
        .log_file_max_size_kilobytes = 2000U,
        .no_pingers = 6U,
        .reflector_ping_interval_microseconds = 300000U,
        .download_average_owd_delta_maximum_adjust_up_microseconds = 10000U,
        .upload_average_owd_delta_maximum_adjust_up_microseconds = 10000U,
        .download_owd_delta_delay_threshold_microseconds = 30000U,
        .upload_owd_delta_delay_threshold_microseconds = 30000U,
        .download_average_owd_delta_maximum_adjust_down_microseconds = 60000U,
        .upload_average_owd_delta_maximum_adjust_down_microseconds = 60000U,
        .minimum_download_rate_bits_per_second = 5000000U,
        .base_download_rate_bits_per_second = 20000000U,
        .maximum_download_rate_bits_per_second = 80000000U,
        .minimum_upload_rate_bits_per_second = 5000000U,
        .base_upload_rate_bits_per_second = 20000000U,
        .maximum_upload_rate_bits_per_second = 35000000U,
        .connection_active_threshold_bits_per_second = 2000000U,
        .sustained_idle_sleep_threshold_microseconds = 60000000U,
        .log_file_buffer_timeout_microseconds = 500000U,
        .irtt_session_duration_minutes = 10U,
        .monitor_achieved_rates_interval_microseconds = 200000U,
        .monitor_cpu_usage_interval_microseconds = 2000000U,
        .bufferbloat_detection_window = 6U,
        .bufferbloat_detection_threshold = 3U,
        .alpha_baseline_increase_per_million = 1000U,
        .alpha_baseline_decrease_per_million = 900000U,
        .alpha_delta_ewma_per_million = 95000U,
        .shaper_rate_minimum_adjust_down_bufferbloat_per_million = 990000U,
        .shaper_rate_maximum_adjust_down_bufferbloat_per_million = 750000U,
        .shaper_rate_minimum_adjust_up_load_high_per_million = 1000000U,
        .shaper_rate_maximum_adjust_up_load_high_per_million = 1040000U,
        .shaper_rate_adjust_down_load_low_per_million = 990000U,
        .shaper_rate_adjust_up_load_low_per_million = 1010000U,
        .high_load_threshold_per_million = 750000U,
        .bufferbloat_refractory_period_microseconds = 300000U,
        .decay_refractory_period_microseconds = 1000000U,
        .reflector_health_check_interval_microseconds = 1000000U,
        .reflector_response_deadline_microseconds = 1000000U,
        .reflector_misbehaving_detection_window = 60U,
        .reflector_misbehaving_detection_threshold = 3U,
        .reflector_replacement_interval_minutes = 60U,
        .reflector_comparison_interval_minutes = 1U,
        .reflector_sum_owd_baselines_delta_threshold_microseconds = 20000U,
        .reflector_owd_delta_ewma_delta_threshold_microseconds = 10000U,
        .stall_detection_threshold = 5U,
        .connection_stall_threshold_bits_per_second = 10000U,
        .global_ping_response_timeout_microseconds = 10000000U,
        .interface_up_check_interval_microseconds = 10000000U
    };

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

    section = uci_lookup_section(context, package, UCI_SECTION);
    if (section == NULL || strcmp(section->type, UCI_SECTION_TYPE) != 0) {
        error_set(
            error,
            error_size,
            "missing config cake_adapt 'main' section"
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
    if (package != NULL) {
        uci_unload(context, package);
    }
    uci_free_context(context);
    return result;
}
