#ifndef CONFIG_DESCRIBE_H_INCLUDED
#define CONFIG_DESCRIBE_H_INCLUDED

#include "config/config.h"

/* -L: every option's name, one per line on stdout; nonzero when writing failed. */
int config_print_options(void);

/* -D: "name=value" for each option, as a section that sets none runs. */
int config_print_defaults(void);

/* The notice that a shell config_file was imported into UCI, when one was. */
void config_log_import(const struct config *config);

/* What was loaded: interfaces, reflectors, timing, logging and the adjusted rates. */
void config_log_loaded(const struct config *config, const char *log_path);

/* Whether the instance controls CAKE, and in which directions, or only observes. */
void config_log_control_mode(const struct config *config);

#endif
