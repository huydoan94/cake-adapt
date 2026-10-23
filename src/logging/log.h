#ifndef LOG_H_INCLUDED
#define LOG_H_INCLUDED

#include "platform/cpu.h"
#include "platform/memory.h"

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
	uint64_t download_achieved_rate_bps;
	uint64_t upload_achieved_rate_bps;
	uint64_t cake_download_rate_bps;
	uint64_t cake_upload_rate_bps;
};

struct log_data_record {
	uint64_t download_achieved_rate_bps;
	uint64_t upload_achieved_rate_bps;
	uint64_t download_load_ratio_e6;
	uint64_t upload_load_ratio_e6;
	/* The pinger's timestamp token, verbatim. */
	const char *icmp_timestamp;
	const char *reflector;
	uint64_t sequence;
	int64_t download_owd_baseline_us;
	int64_t download_owd_us;
	int64_t download_owd_delta_ewma_us;
	int64_t download_owd_delta_us;
	uint64_t download_adjust_delay_threshold_us;
	int64_t upload_owd_baseline_us;
	int64_t upload_owd_us;
	int64_t upload_owd_delta_ewma_us;
	int64_t upload_owd_delta_us;
	uint64_t upload_adjust_delay_threshold_us;
	unsigned int download_sum_delays;
	int64_t download_average_owd_delta_us;
	uint64_t download_maximum_adjust_up_threshold_us;
	uint64_t download_maximum_adjust_down_threshold_us;
	unsigned int upload_sum_delays;
	int64_t upload_average_owd_delta_us;
	uint64_t upload_maximum_adjust_up_threshold_us;
	uint64_t upload_maximum_adjust_down_threshold_us;
	const char *download_load_condition;
	const char *upload_load_condition;
	uint64_t cake_download_rate_bps;
	uint64_t cake_upload_rate_bps;
};

struct log_summary_record {
	uint64_t download_achieved_rate_bps;
	uint64_t upload_achieved_rate_bps;
	unsigned int download_sum_delays;
	unsigned int upload_sum_delays;
	int64_t download_average_owd_delta_us;
	int64_t upload_average_owd_delta_us;
	const char *download_load_condition;
	const char *upload_load_condition;
	uint64_t cake_download_rate_bps;
	uint64_t cake_upload_rate_bps;
};

struct log_reflector_record {
	const char *reflector;
	int64_t minimum_sum_owd_baselines_us;
	int64_t sum_owd_baselines_us;
	uint64_t sum_owd_baselines_delta_us;
	uint64_t sum_owd_baselines_delta_threshold_us;
	int64_t minimum_download_delta_ewma_us;
	int64_t download_delta_ewma_us;
	int64_t download_delta_ewma_delta_us;
	uint64_t delta_ewma_delta_threshold_us;
	int64_t minimum_upload_delta_ewma_us;
	int64_t upload_delta_ewma_us;
	int64_t upload_delta_ewma_delta_us;
};

void log_init(const char *identifier, bool foreground);

/*
 * Stops logging: drains the writer within a bound and closes the file and
 * syslog. False when the writer is stuck (logd or storage not responding);
 * the caller then exits with _exit(), as a normal exit would wait for it.
 */
bool log_close(void);

/* Rotation, buffering and export of the detailed log file; zero disables a limit. */
struct log_file_settings {
	uint64_t maximum_time_us;
	uint64_t maximum_size_bytes;
	uint64_t buffer_timeout_us;
	bool compress_exports;
};

int log_set_file(const char *path, const struct log_file_settings *settings);

void log_set_level(enum log_level level);

void log_tick(void);

int log_export_file(char *export_path, size_t export_path_size);

int log_reset_file(void);

/*
 * Moves every write to the log file, stdout and syslog, with rotation,
 * export and reset, to a writer thread: the caller only queues lines and
 * never waits for storage or logd. The queue is a bounded FIFO: when it is
 * full, the oldest lines make room for the newest and are counted, and the
 * log says how many were overwritten. Without it (a configuration check, the
 * tests), logging writes directly.
 */
int log_start_writer(char *error, size_t error_size);

/* SIGUSR1 and SIGUSR2: export or reset the log file, on the writer when it runs. */
void log_request_export(void);
void log_request_reset(void);

void log_set_debug_syslog(bool enabled);

/* Also writes warnings and errors to stderr, for a configuration check run by hand. */
void log_problems_to_stderr(void);

/* The record types written to the log file; their headers start every rotated file. */
struct log_records {
	bool data;
	bool load;
	bool reflector;
	bool summary;
	/* cake-adapt only: per-direction queues measured from TCP timestamps. */
	bool tcp_queue;
	/* cake-adapt only: the experimental TCP timestamp injection's counters. */
	bool tcp_inject;
	/* cake-adapt only: the daemon's own memory use. */
	bool memory;
};

void log_print_headers(const struct log_records *records);

/* cake-adapt only: per-direction queues measured from TCP timestamps. */
struct log_tcp_queue_record {
	bool download_valid;
	bool upload_valid;
	int64_t download_queue_us;
	int64_t upload_queue_us;
};

void log_tcp_queue(const struct log_tcp_queue_record *record);

/* cake-adapt only: TCP timestamp injection, cumulative since it was loaded. */
struct log_tcp_inject_record {
	uint64_t injected;
	uint64_t skipped;
	uint64_t accepted;
	uint64_t stalled;
};

void log_tcp_inject(const struct log_tcp_inject_record *record);

void log_memory(const struct memory_sample *sample);

void log_load(const struct log_load_record *record);

void log_data(const struct log_data_record *record);

void log_summary(const struct log_summary_record *record);

void log_reflector(const struct log_reflector_record *record);

void log_print_cpu_headers(
	const struct cpu_sample *sample,
	bool output_cpu_stats,
	bool output_cpu_raw_stats
);

void log_cpu(const struct cpu_sample *sample, const struct cpu_busy *usage);

void log_cpu_raw(const struct cpu_sample *sample);

void log_shaper(const char *interface, uint64_t rate_bps);

void log_system_message(const char *format, ...) __attribute__((format(printf, 1, 2)));

void log_message(enum log_level level, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

#endif
