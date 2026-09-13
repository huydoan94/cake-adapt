#ifndef SQM_MON_CONFIG_H
#define SQM_MON_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <net/if.h>
#include <netinet/in.h>

#define SQM_MON_LOG_FILE_SIZE 256U
#define SQM_MON_LATENCY_TARGET_SIZE INET_ADDRSTRLEN

struct sqm_mon_config {
    bool enabled;
    bool adjust_download;
    bool adjust_upload;
    bool output_processing_stats;
    bool output_load_stats;
    bool output_summary_stats;
    bool output_cake_changes;
    bool debug;
    bool log_debug_messages_to_syslog;
    char interface[IF_NAMESIZE];
    char ingress_interface[IF_NAMESIZE];
    char latency_target[SQM_MON_LATENCY_TARGET_SIZE];
    char log_file[SQM_MON_LOG_FILE_SIZE];
    uint64_t minimum_download_rate_bits_per_second;
    uint64_t base_download_rate_bits_per_second;
    uint64_t maximum_download_rate_bits_per_second;
    uint64_t minimum_upload_rate_bits_per_second;
    uint64_t base_upload_rate_bits_per_second;
    uint64_t maximum_upload_rate_bits_per_second;
    uint64_t connection_active_threshold_bits_per_second;
};

int config_load(
    struct sqm_mon_config *config,
    const char *config_directory,
    char *error,
    size_t error_size
);

#endif
