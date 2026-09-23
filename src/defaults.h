#ifndef DEFAULTS_H_INCLUDED
#define DEFAULTS_H_INCLUDED

#include "config.h"
#include "constants.h"

#define DEFAULT_SECTION "main"
#define DEFAULT_CONFIG_PATH "/etc/config/" UCI_PACKAGE
#define DEFAULT_LOG_DIRECTORY "/var/log"
#define DEFAULT_LOG_FILE_BASE PROGRAM_NAME
#define DEFAULT_FPING_TIMEOUT_MILLISECONDS "10000"

void defaults_apply(struct config *config);

#endif
