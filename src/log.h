#ifndef SQM_MON_LOG_H
#define SQM_MON_LOG_H

#include "cpu.h"

#include <stdbool.h>
#include <stdint.h>

enum log_level {
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_NOTICE,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG
};

struct log_load_record {
    uint64_t download_achieved_rate_kbps;
    uint64_t upload_achieved_rate_kbps;
    uint64_t cake_download_rate_kbps;
    uint64_t cake_upload_rate_kbps;
};

struct log_data_record {
    uint64_t download_achieved_rate_kbps;
    uint64_t upload_achieved_rate_kbps;
    unsigned int download_load_percent;
    unsigned int upload_load_percent;
    uint64_t icmp_timestamp_microseconds;
    const char *reflector;
    uint64_t sequence;
    uint32_t download_owd_baseline_microseconds;
    uint32_t download_owd_microseconds;
    int64_t download_owd_delta_ewma_microseconds;
    int64_t download_owd_delta_microseconds;
    uint64_t download_adjust_delay_threshold_microseconds;
    uint32_t upload_owd_baseline_microseconds;
    uint32_t upload_owd_microseconds;
    int64_t upload_owd_delta_ewma_microseconds;
    int64_t upload_owd_delta_microseconds;
    uint64_t upload_adjust_delay_threshold_microseconds;
    unsigned int download_sum_delays;
    int64_t download_average_owd_delta_microseconds;
    uint64_t download_maximum_adjust_up_threshold_microseconds;
    uint64_t download_maximum_adjust_down_threshold_microseconds;
    unsigned int upload_sum_delays;
    int64_t upload_average_owd_delta_microseconds;
    uint64_t upload_maximum_adjust_up_threshold_microseconds;
    uint64_t upload_maximum_adjust_down_threshold_microseconds;
    const char *download_load_condition;
    const char *upload_load_condition;
    uint64_t cake_download_rate_kbps;
    uint64_t cake_upload_rate_kbps;
};

struct log_summary_record {
    uint64_t download_achieved_rate_kbps;
    uint64_t upload_achieved_rate_kbps;
    unsigned int download_sum_delays;
    unsigned int upload_sum_delays;
    int64_t download_average_owd_delta_microseconds;
    int64_t upload_average_owd_delta_microseconds;
    const char *download_load_condition;
    const char *upload_load_condition;
    uint64_t cake_download_rate_kbps;
    uint64_t cake_upload_rate_kbps;
};

struct log_reflector_record {
    const char *reflector;
    uint64_t minimum_sum_owd_baselines_microseconds;
    uint64_t sum_owd_baselines_microseconds;
    uint64_t sum_owd_baselines_delta_microseconds;
    uint64_t sum_owd_baselines_delta_threshold_microseconds;
    int64_t minimum_download_delta_ewma_microseconds;
    int64_t download_delta_ewma_microseconds;
    int64_t download_delta_ewma_delta_microseconds;
    uint64_t delta_ewma_delta_threshold_microseconds;
    int64_t minimum_upload_delta_ewma_microseconds;
    int64_t upload_delta_ewma_microseconds;
    int64_t upload_delta_ewma_delta_microseconds;
};

void log_init(
    const char *identifier,
    bool foreground
);

void log_close(void);

int log_set_file(
    const char *path,
    uint64_t maximum_time_minutes,
    uint64_t maximum_size_kilobytes,
    uint64_t buffer_timeout_microseconds,
    bool compress_exports
);

int log_set_level(const char *level);

void log_tick(void);

int log_export_file(
    char *export_path,
    size_t export_path_size
);

int log_reset_file(void);

void log_set_debug_syslog(bool enabled);

void log_print_headers(
    bool output_processing_stats,
    bool output_load_stats,
    bool output_reflector_stats,
    bool output_summary_stats
);

uint64_t log_realtime_microseconds(void);

void log_load(const struct log_load_record *record);

void log_data(const struct log_data_record *record);

void log_summary(const struct log_summary_record *record);

void log_reflector(const struct log_reflector_record *record);

void log_print_cpu_headers(
    const struct cpu_sample *sample,
    bool output_cpu_stats,
    bool output_cpu_raw_stats
);

void log_cpu(
    const struct cpu_sample *sample,
    const unsigned int *usage
);

void log_cpu_raw(const struct cpu_sample *sample);

void log_shaper(
    const char *interface,
    uint64_t rate_kbps
);

void log_system_message(
    const char *format,
    ...
) __attribute__((format(printf, 1, 2)));

void log_message(
    enum log_level level,
    const char *format,
    ...
) __attribute__((format(printf, 2, 3)));

#endif
