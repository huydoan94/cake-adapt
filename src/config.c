#include "config.h"

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
    const char *enabled;
    const char *interface;
    const char *log_level;

    enabled = uci_lookup_option_string(context, section, "enabled");
    if (enabled != NULL && parse_boolean(enabled, &config->enabled) != 0) {
        set_error(error, error_size, "option 'enabled' is not a boolean");
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

    if (config->enabled && config->interface[0] == '\0') {
        set_error(
            error,
            error_size,
            "option 'interface' is required when sqm-mon is enabled"
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
        .interface = "",
        .log_level = "info"
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
