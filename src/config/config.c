#define _GNU_SOURCE

#include "config/config.h"
#include "config/options.h"
#include "config/defaults.h"
#include "common/error.h"
#include "common/text.h"
#include "platform/path.h"
#include "constants/cake.h"
#include "constants/options.h"
#include "constants/text.h"
#include "constants/units.h"

#include <libubox/utils.h>

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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

/* The section being loaded, the configuration it fills and where a failure is described. */
struct loader {
	struct uci_context *context;
	struct uci_section *section;
	struct config *config;
	char *error;
	size_t error_size;
};

static int copy_option(
	const struct loader *loader,
	const char *name,
	char *destination,
	size_t destination_size,
	const char *value
)
{
	size_t length = strlen(value);

	if (length >= destination_size)
		return error_set(loader->error, loader->error_size, "option '%s' is too long", name);

	memcpy(destination, value, length + 1U);
	return 0;
}

static int resolve_interfaces(struct config *config, char *error, size_t error_size)
{
	bool upload_configured = config->ul_if[0] != '\0';
	bool download_configured = config->dl_if[0] != '\0';

	config->interface_overridden = false;
	if (upload_configured != download_configured) {
		return error_set(
			error,
			error_size,
			"options 'ul_if' and 'dl_if' must be configured together"
		);
	}
	/* The mismatch check above guarantees dl_if is configured here too. */
	if (upload_configured) {
		config->interface_overridden = config->interface[0] != '\0';
		memcpy(config->interface, config->ul_if, sizeof(config->interface));
		memcpy(config->ingress_interface, config->dl_if, sizeof(config->ingress_interface));
		return 0;
	}

	config->ingress_interface[0] = '\0';
	if (config->interface[0] == '\0')
		return 0;

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

static int parse_boolean(const char *value, bool *result)
{
	if (strcmp(value, BOOLEAN_TRUE_NUMERIC) == 0 || strcasecmp(value, BOOLEAN_TRUE) == 0 ||
	    strcasecmp(value, BOOLEAN_YES) == 0 || strcasecmp(value, BOOLEAN_ON) == 0) {
		*result = true;
		return 0;
	}

	if (strcmp(value, BOOLEAN_FALSE_NUMERIC) == 0 || strcasecmp(value, BOOLEAN_FALSE) == 0 ||
	    strcasecmp(value, BOOLEAN_NO) == 0 || strcasecmp(value, BOOLEAN_OFF) == 0) {
		*result = false;
		return 0;
	}

	return -1;
}

/* A scalar option's value, or NULL when the section does not set it. */
static int lookup_string_option(const struct loader *loader, const char *name, const char **value)
{
	struct uci_option *option = uci_lookup_option(loader->context, loader->section, name);

	*value = NULL;
	if (option == NULL)
		return 0;
	if (option->type != UCI_TYPE_STRING) {
		return error_set(
			loader->error,
			loader->error_size,
			"option '%s' must be a scalar UCI option, not a list",
			name
		);
	}
	*value = option->v.string;
	return 0;
}

static int parse_scaled_decimal(
	const struct loader *loader,
	const struct option_binding *option,
	const char *value,
	uint64_t *result
)
{
	const char *character = value + strspn(value, DECIMAL_DIGITS);
	uint64_t scaled_value;
	uint64_t fractional_place = option->scale;

	if (value[0] == '\0')
		return error_set(
			loader->error,
			loader->error_size,
			"option '%s' is empty",
			option->name
		);
	if (character == value)
		goto invalid;
	if (!parse_unsigned(value, character, &scaled_value) ||
	    scaled_value > UINT64_MAX / option->scale)
		goto too_large;
	scaled_value *= option->scale;

	if (*character != '\0') {
		/* A point followed by one or more digits. */
		if (*character != '.' || character[1] == '\0' ||
		    character[1 + strspn(character + 1, DECIMAL_DIGITS)] != '\0') {
			goto invalid;
		}
		for (character++; *character != '\0'; character++) {
			uint64_t digit = (uint64_t)(*character - '0');

			/* The next place would not be a whole number of stored units. */
			if (fractional_place % 10U != 0U) {
				if (digit != 0U) {
					return error_set(
						loader->error,
						loader->error_size,
						"option '%s' has more precision than cake-adapt stores",
						option->name
					);
				}
				continue;
			}
			fractional_place /= 10U;
			if (scaled_value > UINT64_MAX - digit * fractional_place)
				goto too_large;
			scaled_value += digit * fractional_place;
		}
	}
	*result = scaled_value;
	return 0;

invalid:
	return error_set(
		loader->error,
		loader->error_size,
		"option '%s' is not a non-negative decimal",
		option->name
	);
too_large:
	return error_set(
		loader->error,
		loader->error_size,
		"option '%s' is too large",
		option->name
	);
}

static int
load_option(const struct loader *loader, const struct option_binding *option, const char *value)
{
	char *destination = (char *)loader->config + option->offset;

	switch (option->type) {
	case OPTION_TYPE_BOOLEAN:
		if (parse_boolean(value, (bool *)destination) != 0)
			return error_set(
				loader->error,
				loader->error_size,
				"option '%s' is not a boolean",
				option->name
			);
		return 0;
	case OPTION_TYPE_STRING:
		return copy_option(loader, option->name, destination, option->size, value);
	case OPTION_TYPE_SCALED:
		return parse_scaled_decimal(loader, option, value, (uint64_t *)destination);
	}
	return 0;
}

static int load_options(const struct loader *loader)
{
	size_t index;

	for (index = 0U; index < config_option_count(); index++) {
		const struct option_binding *option = config_option(index);
		const char *value;

		if (lookup_string_option(loader, option->name, &value) != 0)
			return -1;
		if (value != NULL && load_option(loader, option, value) != 0)
			return -1;
	}
	return 0;
}

static int copy_reflector(const struct loader *loader, const char *reflector)
{
	struct config *config = loader->config;
	uint64_t index = config->reflector_count;

	if (index >= CONFIG_MAX_REFLECTORS) {
		return error_set(
			loader->error,
			loader->error_size,
			"option 'reflectors' contains more than %u entries",
			CONFIG_MAX_REFLECTORS
		);
	}
	if (copy_option(
		    loader,
		    OPTION_REFLECTORS,
		    config->reflectors[index],
		    sizeof(config->reflectors[index]),
		    reflector
	    ) != 0) {
		return -1;
	}
	config->reflector_count++;
	return 0;
}

static int load_reflectors(const struct loader *loader)
{
	struct uci_option *option =
		uci_lookup_option(loader->context, loader->section, OPTION_REFLECTORS);
	struct uci_element *element;
	struct uci_list *list;

	if (option == NULL)
		return 0;
	if (option->type != UCI_TYPE_LIST)
		return error_set(
			loader->error,
			loader->error_size,
			"option 'reflectors' must be a UCI list"
		);

	list = &option->v.list;
	loader->config->reflector_count = 0U;
	uci_foreach_element(list, element) {
		if (copy_reflector(loader, element->name) != 0)
			return -1;
	}
	return 0;
}

static int load_section(const struct loader *loader)
{
	struct config *config = loader->config;
	bool reflectors_configured =
		uci_lookup_option(loader->context, loader->section, OPTION_REFLECTORS) != NULL;
	bool no_pingers_configured =
		uci_lookup_option(loader->context, loader->section, OPTION_NO_PINGERS) != NULL;
	/* Legacy single-target input is needed only while loading configuration. */
	char latency_target[CONFIG_REFLECTOR_SIZE] = { 0 };
	const char *latency_target_value;

	if (lookup_string_option(loader, OPTION_LATENCY_TARGET, &latency_target_value) != 0 ||
	    (latency_target_value != NULL && copy_option(
						     loader,
						     OPTION_LATENCY_TARGET,
						     latency_target,
						     sizeof(latency_target),
						     latency_target_value
					     ) != 0)) {
		return -1;
	}

	if (load_options(loader) != 0 || load_reflectors(loader) != 0 ||
	    resolve_interfaces(config, loader->error, loader->error_size) != 0)
		return -1;

	if (!reflectors_configured && latency_target[0] != '\0') {
		config->reflector_count = 0U;
		if (copy_reflector(loader, latency_target) != 0)
			return -1;
		if (!no_pingers_configured)
			config->no_pingers = 1U;
	}

	return config_validate(config, loader->error, loader->error_size);
}

/* A relative log directory is taken from where the daemon starts, made absolute. */
static int absolute_log_directory(struct config *config, char *error, size_t error_size)
{
	char directory[sizeof(config->log_file_path_override)];

	if (config->log_file_path_override[0] == '\0')
		return 0;
	if (path_absolute(config->log_file_path_override, directory, sizeof(directory)) != 0) {
		return error_set(
			error,
			error_size,
			"option '%s': %s",
			OPTION_LOG_FILE_PATH_OVERRIDE,
			strerror(errno)
		);
	}
	memcpy(config->log_file_path_override, directory, sizeof(directory));
	return 0;
}

int config_check_shared_file(
	const struct config *config,
	const struct stat *status,
	const char *shared_path,
	char *error,
	size_t error_size
)
{
	struct stat shared;
	const char *option = NULL;

	if (config->ping_prefix_string[0] != '\0')
		option = OPTION_PING_PREFIX_STRING;
	else if (config->ping_extra_args[0] != '\0')
		option = OPTION_PING_EXTRA_ARGS;
	else if (config->log_file_path_override[0] != '\0')
		option = OPTION_LOG_FILE_PATH_OVERRIDE;
	if (option == NULL || stat(shared_path, &shared) != 0 || shared.st_dev != status->st_dev ||
	    shared.st_ino != status->st_ino)
		return 0;
	return error_set(
		error,
		error_size,
		"option '%s' is refused in %s, which LuCI can change; move it to /etc/cake-adapt/exec",
		option,
		shared_path
	);
}

int config_load(
	struct config *config,
	const char *config_path,
	const char *section_name,
	char *error,
	size_t error_size
)
{
	struct loader loader = {
		.config = config,
		.error = error,
		.error_size = error_size,
	};
	struct uci_context *context;
	struct uci_package *package = NULL;
	struct uci_section *section;
	struct stat status;
	int result = -1;

	if (section_name == NULL || section_name[0] == '\0')
		return error_set(error, error_size, "UCI section name is empty");
	if (stat(config_path, &status) != 0) {
		return error_set(
			error,
			error_size,
			"cannot read configuration file '%s': %s",
			config_path,
			strerror(errno)
		);
	}
	if (!S_ISREG(status.st_mode))
		return error_set(error, error_size, "'%s' is not a configuration file", config_path);

	defaults_apply(config);

	context = uci_alloc_context();
	if (context == NULL)
		return error_set(error, error_size, "could not allocate a UCI context");

	if (uci_load(context, config_path, &package) != UCI_OK) {
		char *uci_error = NULL;

		uci_get_errorstr(context, &uci_error, config_path);
		error_set(
			error,
			error_size,
			"%s",
			uci_error != NULL ? uci_error : "could not load UCI configuration"
		);
		free(uci_error);
		goto done;
	}

	loader.context = context;
	section = uci_lookup_section(context, package, section_name);
	loader.section = section;
	if (section == NULL || strcmp(section->type, UCI_SECTION_TYPE) != 0) {
		error_set(error, error_size, "missing config cake_adapt '%s' section", section_name);
		goto done;
	}

	result = load_section(&loader);
	if (result == 0)
		result = absolute_log_directory(config, error, error_size);
	if (result == 0)
		result = config_check_shared_file(
			config,
			&status,
			DEFAULT_CONFIG_PATH,
			error,
			error_size
		);

done:
	/* libuci owns and releases every package loaded into this context. */
	uci_free_context(context);
	return result;
}
