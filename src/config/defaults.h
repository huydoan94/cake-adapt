#ifndef CONFIG_DEFAULTS_H_INCLUDED
#define CONFIG_DEFAULTS_H_INCLUDED

#include "constants/options.h"
#include "constants/program.h"

struct config;

#define DEFAULT_SECTION "main"
#define DEFAULT_CONFIG_PATH "/etc/config/" UCI_PACKAGE
#define DEFAULT_LOG_DIRECTORY "/var/log"
#define DEFAULT_LOG_FILE_BASE PROGRAM_NAME
#define DEFAULT_MQTT_CA_FILE "/etc/ssl/certs/ca-certificates.crt"

/* TCP-timestamp queue estimation. */

void defaults_apply(struct config *config);

#endif
