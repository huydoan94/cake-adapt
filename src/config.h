#ifndef SQM_MON_CONFIG_H
#define SQM_MON_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <net/if.h>

#define SQM_MON_LOG_LEVEL_SIZE 16U

struct sqm_mon_config {
    bool enabled;
    char interface[IF_NAMESIZE];
    char log_level[SQM_MON_LOG_LEVEL_SIZE];
};

int config_load(
    struct sqm_mon_config *config,
    const char *config_directory,
    char *error,
    size_t error_size
);

#endif
