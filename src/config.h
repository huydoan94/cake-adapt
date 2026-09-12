#ifndef SQM_MON_CONFIG_H
#define SQM_MON_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <net/if.h>
#include <netinet/in.h>

#define SQM_MON_LOG_LEVEL_SIZE 16U
#define SQM_MON_LOG_FILE_SIZE 256U
#define SQM_MON_LATENCY_TARGET_SIZE INET_ADDRSTRLEN

struct sqm_mon_config {
    bool enabled;
    char interface[IF_NAMESIZE];
    char ingress_interface[IF_NAMESIZE];
    char latency_target[SQM_MON_LATENCY_TARGET_SIZE];
    char log_file[SQM_MON_LOG_FILE_SIZE];
    char log_level[SQM_MON_LOG_LEVEL_SIZE];
};

int config_load(
    struct sqm_mon_config *config,
    const char *config_directory,
    char *error,
    size_t error_size
);

#endif
