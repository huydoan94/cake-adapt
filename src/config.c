#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Current libuci headers contain inline helpers that trigger -Wsign-conversion.
 * Keep strict conversion warnings for sqm-mon while isolating that external
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

#define SQM_MON_UCI_PACKAGE "sqm-mon"
#define SQM_MON_UCI_SECTION "main"
#define SQM_MON_UCI_SECTION_TYPE "sqm_mon"

static void set_error(
    char *error,
    size_t error_size,
    const char *format,
    ...
)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

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
        set_error(
            error,
            error_size,
            "option '%s' is too long",
            option_name
        );
        return -1;
    }

    return 0;
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

static int parse_rate_kbps(
    const char *value,
    uint64_t *rate_bits_per_second,
    const char *option_name,
    char *error,
    size_t error_size
)
{
    const char *character;
    char *end;
    uintmax_t rate_kbps;

    if (value[0] == '\0') {
        set_error(error, error_size, "option '%s' is empty", option_name);
        return -1;
    }
    for (character = value; *character != '\0'; character++) {
        if (!isdigit((unsigned char)*character)) {
            set_error(
                error,
                error_size,
                "option '%s' is not an unsigned integer",
                option_name
            );
            return -1;
        }
    }

    errno = 0;
    rate_kbps = strtoumax(value, &end, 10);
    if (errno == ERANGE || *end != '\0' ||
        rate_kbps > UINT64_MAX / 1000U) {
        set_error(error, error_size, "option '%s' is too large", option_name);
        return -1;
    }

    *rate_bits_per_second = (uint64_t)rate_kbps * 1000U;
    return 0;
}

static int load_rate_option(
    struct uci_context *context,
    struct uci_section *section,
    const char *option_name,
    uint64_t *rate_bits_per_second,
    char *error,
    size_t error_size
)
{
    const char *value = uci_lookup_option_string(
        context,
        section,
        option_name
    );

    if (value == NULL) {
        return 0;
    }
    return parse_rate_kbps(
        value,
        rate_bits_per_second,
        option_name,
        error,
        error_size
    );
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
        set_error(
            error,
            error_size,
            "%s min/base/max rates are required together",
            direction
        );
        return -1;
    }
    if (minimum > base || base > maximum) {
        set_error(
            error,
            error_size,
            "%s rates must satisfy minimum <= base <= maximum",
            direction
        );
        return -1;
    }
    return 0;
}

static bool log_level_is_valid(const char *value)
{
    return strcasecmp(value, "debug") == 0 ||
        strcasecmp(value, "info") == 0 ||
        strcasecmp(value, "notice") == 0 ||
        strcasecmp(value, "warning") == 0 ||
        strcasecmp(value, "error") == 0;
}

static struct uci_section *find_main_section(struct uci_package *package)
{
    struct uci_element *element;

    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);

        if (strcmp(section->e.name, SQM_MON_UCI_SECTION) == 0 &&
            strcmp(section->type, SQM_MON_UCI_SECTION_TYPE) == 0) {
            return section;
        }
    }

    return NULL;
}

static int load_section(
    struct uci_context *context,
    struct uci_section *section,
    struct sqm_mon_config *config,
    char *error,
    size_t error_size
)
{
    const char *adjust_download;
    const char *adjust_upload;
    const char *enabled;
    const char *ingress_interface;
    const char *interface;
    const char *latency_target;
    const char *log_file;
    const char *log_level;

    enabled = uci_lookup_option_string(context, section, "enabled");
    if (enabled != NULL && parse_boolean(enabled, &config->enabled) != 0) {
        set_error(error, error_size, "option 'enabled' is not a boolean");
        return -1;
    }

    adjust_download = uci_lookup_option_string(
        context,
        section,
        "adjust_dl_shaper_rate"
    );
    if (adjust_download != NULL &&
        parse_boolean(adjust_download, &config->adjust_download) != 0) {
        set_error(
            error,
            error_size,
            "option 'adjust_dl_shaper_rate' is not a boolean"
        );
        return -1;
    }

    adjust_upload = uci_lookup_option_string(
        context,
        section,
        "adjust_ul_shaper_rate"
    );
    if (adjust_upload != NULL &&
        parse_boolean(adjust_upload, &config->adjust_upload) != 0) {
        set_error(
            error,
            error_size,
            "option 'adjust_ul_shaper_rate' is not a boolean"
        );
        return -1;
    }

    interface = uci_lookup_option_string(context, section, "interface");
    if (interface != NULL &&
        copy_option(
            config->interface,
            sizeof(config->interface),
            interface,
            "interface",
            error,
            error_size
        ) != 0) {
        return -1;
    }

    ingress_interface = uci_lookup_option_string(
        context,
        section,
        "ingress_interface"
    );
    if (ingress_interface != NULL &&
        copy_option(
            config->ingress_interface,
            sizeof(config->ingress_interface),
            ingress_interface,
            "ingress_interface",
            error,
            error_size
        ) != 0) {
        return -1;
    }

    latency_target = uci_lookup_option_string(
        context,
        section,
        "latency_target"
    );
    if (latency_target != NULL &&
        copy_option(
            config->latency_target,
            sizeof(config->latency_target),
            latency_target,
            "latency_target",
            error,
            error_size
        ) != 0) {
        return -1;
    }

    log_file = uci_lookup_option_string(context, section, "log_file");
    if (log_file != NULL &&
        copy_option(
            config->log_file,
            sizeof(config->log_file),
            log_file,
            "log_file",
            error,
            error_size
        ) != 0) {
        return -1;
    }

    log_level = uci_lookup_option_string(context, section, "log_level");
    if (log_level != NULL) {
        if (!log_level_is_valid(log_level)) {
            set_error(error, error_size, "option 'log_level' is invalid");
            return -1;
        }

        if (copy_option(
                config->log_level,
                sizeof(config->log_level),
                log_level,
                "log_level",
                error,
                error_size
            ) != 0) {
            return -1;
        }
    }

    if (load_rate_option(
            context,
            section,
            "min_dl_shaper_rate_kbps",
            &config->minimum_download_rate_bits_per_second,
            error,
            error_size
        ) != 0 ||
        load_rate_option(
            context,
            section,
            "base_dl_shaper_rate_kbps",
            &config->base_download_rate_bits_per_second,
            error,
            error_size
        ) != 0 ||
        load_rate_option(
            context,
            section,
            "max_dl_shaper_rate_kbps",
            &config->maximum_download_rate_bits_per_second,
            error,
            error_size
        ) != 0 ||
        load_rate_option(
            context,
            section,
            "min_ul_shaper_rate_kbps",
            &config->minimum_upload_rate_bits_per_second,
            error,
            error_size
        ) != 0 ||
        load_rate_option(
            context,
            section,
            "base_ul_shaper_rate_kbps",
            &config->base_upload_rate_bits_per_second,
            error,
            error_size
        ) != 0 ||
        load_rate_option(
            context,
            section,
            "max_ul_shaper_rate_kbps",
            &config->maximum_upload_rate_bits_per_second,
            error,
            error_size
        ) != 0) {
        return -1;
    }

    if (config->enabled && config->interface[0] == '\0') {
        set_error(
            error,
            error_size,
            "option 'interface' is required when sqm-mon is enabled"
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
    if (config->adjust_download && config->ingress_interface[0] == '\0') {
        set_error(
            error,
            error_size,
            "option 'ingress_interface' is required for download adjustment"
        );
        return -1;
    }
    if ((config->adjust_download || config->adjust_upload) &&
        config->latency_target[0] == '\0') {
        set_error(
            error,
            error_size,
            "option 'latency_target' is required for rate adjustment"
        );
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
        set_error(error, error_size, "configuration destination is null");
        return -1;
    }

    *config = (struct sqm_mon_config) {
        .enabled = false,
        .adjust_download = false,
        .adjust_upload = false,
        .interface = "",
        .ingress_interface = "",
        .latency_target = "",
        .log_file = "",
        .log_level = "info",
        .minimum_download_rate_bits_per_second = 0U,
        .base_download_rate_bits_per_second = 0U,
        .maximum_download_rate_bits_per_second = 0U,
        .minimum_upload_rate_bits_per_second = 0U,
        .base_upload_rate_bits_per_second = 0U,
        .maximum_upload_rate_bits_per_second = 0U
    };

    context = uci_alloc_context();
    if (context == NULL) {
        set_error(error, error_size, "could not allocate a UCI context");
        return -1;
    }

    if (config_directory != NULL) {
        if (uci_set_confdir(context, config_directory) != UCI_OK) {
            set_error(
                error,
                error_size,
                "could not use UCI configuration directory '%s'",
                config_directory
            );
            goto done;
        }
    }

    if (uci_load(context, SQM_MON_UCI_PACKAGE, &package) != UCI_OK) {
        char *uci_error = NULL;

        uci_get_errorstr(context, &uci_error, SQM_MON_UCI_PACKAGE);
        set_error(
            error,
            error_size,
            "%s",
            uci_error != NULL ? uci_error : "could not load UCI configuration"
        );
        free(uci_error);
        goto done;
    }

    section = find_main_section(package);
    if (section == NULL) {
        set_error(
            error,
            error_size,
            "missing config sqm_mon 'main' section"
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
