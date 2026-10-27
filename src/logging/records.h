#ifndef LOGGING_RECORDS_H_INCLUDED
#define LOGGING_RECORDS_H_INCLUDED

/*
 * The structured records, in cake-autorate's analyzer schemas where it has
 * them: written to the log file, stdout and MQTT, never to syslog.
 */

#include "common/direction.h"
#include "platform/cpu.h"
#include "platform/memory.h"

#include <stdbool.h>
#include <stdint.h>

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

struct log_load_record {
	uint64_t achieved_rate_bps[DIRECTION_COUNT];
	uint64_t cake_rate_bps[DIRECTION_COUNT];
};

struct log_data_record {
	uint64_t achieved_rate_bps[DIRECTION_COUNT];
	uint64_t load_ratio_e6[DIRECTION_COUNT];
	/* The pinger's timestamp token, verbatim. */
	const char *icmp_timestamp;
	const char *reflector;
	uint64_t sequence;
	int64_t owd_baseline_us[DIRECTION_COUNT];
	int64_t owd_us[DIRECTION_COUNT];
	int64_t owd_delta_ewma_us[DIRECTION_COUNT];
	int64_t owd_delta_us[DIRECTION_COUNT];
	uint64_t adjust_delay_threshold_us[DIRECTION_COUNT];
	unsigned int sum_delays[DIRECTION_COUNT];
	int64_t average_owd_delta_us[DIRECTION_COUNT];
	uint64_t maximum_adjust_up_threshold_us[DIRECTION_COUNT];
	uint64_t maximum_adjust_down_threshold_us[DIRECTION_COUNT];
	const char *load_condition[DIRECTION_COUNT];
	uint64_t cake_rate_bps[DIRECTION_COUNT];
};

struct log_reflector_record {
	const char *reflector;
	int64_t minimum_sum_owd_baselines_us;
	int64_t sum_owd_baselines_us;
	uint64_t sum_owd_baselines_delta_us;
	uint64_t sum_owd_baselines_delta_threshold_us;
	int64_t minimum_delta_ewma_us[DIRECTION_COUNT];
	int64_t delta_ewma_us[DIRECTION_COUNT];
	int64_t delta_ewma_delta_us[DIRECTION_COUNT];
	uint64_t delta_ewma_delta_threshold_us;
};

/* cake-adapt only: per-direction queues measured from TCP timestamps. */
struct log_tcp_queue_record {
	bool valid[DIRECTION_COUNT];
	int64_t queue_us[DIRECTION_COUNT];
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

/* The SUMMARY record: the DATA record's rates, delays and load columns. */
void log_summary(const struct log_data_record *record);

void log_reflector(const struct log_reflector_record *record);

void log_print_cpu_headers(
	const struct cpu_sample *sample,
	bool output_cpu_stats,
	bool output_cpu_raw_stats
);

void log_cpu(const struct cpu_sample *sample, const struct cpu_busy *usage);

void log_cpu_raw(const struct cpu_sample *sample);

void log_shaper(const char *interface, uint64_t rate_bps);

#endif
