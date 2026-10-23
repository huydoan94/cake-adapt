#define _POSIX_C_SOURCE 200809L

#include "logging/records.h"
#include "logging/output.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "constants/logging.h"
#include "constants/units.h"

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static struct log_records headers;
static char *cpu_header;
static bool header_cpu_raw;
/* The record headers, which the writer copies into a rotated or reset file. */
static pthread_mutex_t header_lock = PTHREAD_MUTEX_INITIALIZER;

/* cake-autorate 3.3.0-PRERELEASE (ac75f493) analyzer schemas. */
static const char data_header[] = "DATA_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
				  " DL_ACHIEVED_RATE_KBPS; UL_ACHIEVED_RATE_KBPS; DL_LOAD_PERCENT;"
				  " UL_LOAD_PERCENT; ICMP_TIMESTAMP; REFLECTOR; SEQUENCE;"
				  " DL_OWD_BASELINE; DL_OWD_US; DL_OWD_DELTA_EWMA_US;"
				  " DL_OWD_DELTA_US; DL_ADJ_DELAY_THR; UL_OWD_BASELINE; UL_OWD_US;"
				  " UL_OWD_DELTA_EWMA_US; UL_OWD_DELTA_US; UL_ADJ_DELAY_THR;"
				  " DL_SUM_DELAYS; DL_AVG_OWD_DELTA_US;"
				  " DL_ADJ_MAX_ADJUST_UP_THR_US; DL_ADJ_MAX_ADJUST_DOWN_THR_US;"
				  " UL_SUM_DELAYS; UL_AVG_OWD_DELTA_US;"
				  " UL_ADJ_MAX_ADJUST_UP_THR_US; UL_ADJ_MAX_ADJUST_DOWN_THR_US;"
				  " DL_LOAD_CONDITION; UL_LOAD_CONDITION; CAKE_DL_RATE_KBPS;"
				  " CAKE_UL_RATE_KBPS";

static const char load_header[] = "LOAD_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
				  " DL_ACHIEVED_RATE_KBPS; UL_ACHIEVED_RATE_KBPS;"
				  " CAKE_DL_RATE_KBPS; CAKE_UL_RATE_KBPS";

static const char summary_header[] =
	"SUMMARY_HEADER; LOG_DATETIME; LOG_TIMESTAMP; DL_ACHIEVED_RATE_KBPS;"
	" UL_ACHIEVED_RATE_KBPS; DL_SUM_DELAYS; UL_SUM_DELAYS;"
	" DL_AVG_OWD_DELTA_US; UL_AVG_OWD_DELTA_US; DL_LOAD_CONDITION;"
	" UL_LOAD_CONDITION; CAKE_DL_RATE_KBPS; CAKE_UL_RATE_KBPS";

static const char reflector_header[] =
	"REFLECTOR_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US; REFLECTOR;"
	" MIN_SUM_OWD_BASELINES_US; SUM_OWD_BASELINES_US;"
	" SUM_OWD_BASELINES_DELTA_US; SUM_OWD_BASELINES_DELTA_THR_US;"
	" MIN_DL_DELTA_EWMA_US; DL_DELTA_EWMA_US; DL_DELTA_EWMA_DELTA_US;"
	" DL_DELTA_EWMA_DELTA_THR; MIN_UL_DELTA_EWMA_US; UL_DELTA_EWMA_US;"
	" UL_DELTA_EWMA_DELTA_US; UL_DELTA_EWMA_DELTA_THR";

/* Not in cake-autorate; analyzers that do not know it skip the record. */
static const char tcp_queue_header[] =
	"TCP_QUEUE_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
	" DL_QUEUE_VALID; DL_QUEUE_US; UL_QUEUE_VALID; UL_QUEUE_US";

/* Not in cake-autorate: TCP timestamp injection counters since it was loaded. */
static const char tcp_inject_header[] =
	"TCP_INJECT_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US; INJECTED; SKIPPED;"
	" ACCEPTED; STALLED";

/* Not in cake-autorate: the daemon's own memory use, in kilobytes. */
static const char memory_header[] = "MEMORY_HEADER; LOG_DATETIME; LOG_TIMESTAMP; PROC_TIME_US;"
				    " RSS_KB; PEAK_RSS_KB; RSS_ANON_KB; DATA_KB";

/* Followed by one "<CPU>_USAGE" column per counter line of /proc/stat. */
static const char cpu_header_prefix[] = "CPU_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME";

static const char cpu_raw_header[] =
	"CPU_RAW_HEADER; LOG_DATETIME; LOG_TIMESTAMP; STATS_READ_TIME; CPU_ID;"
	" USER; NICE; SYSTEM; IDLE; IOWAIT; IRQ; SIRQ; STEAL; GUEST;"
	" GUEST_NICE";

/* Writes the header of each record type in records, in the order every log starts with. */
static void write_record_headers(const struct log_records *records, void (*emit)(const char *))
{
	const struct {
		bool enabled;
		const char *header;
	} record_headers[] = {
		{ records->data, data_header },
		{ records->load, load_header },
		{ records->reflector, reflector_header },
		{ records->summary, summary_header },
		{ records->tcp_queue, tcp_queue_header },
		{ records->tcp_inject, tcp_inject_header },
		{ records->memory, memory_header },
	};

	for (size_t index = 0U; index < ARRAY_SIZE(record_headers); index++)
		if (record_headers[index].enabled)
			emit(record_headers[index].header);
}

void records_write_saved_headers(void (*emit)(const char *))
{
	/* Rotation and reset run in the writer while the main thread may set headers. */
	(void)pthread_mutex_lock(&header_lock);
	write_record_headers(&headers, emit);
	if (cpu_header != NULL)
		emit(cpu_header);
	if (header_cpu_raw)
		emit(cpu_raw_header);
	(void)pthread_mutex_unlock(&header_lock);
}

void records_clear(void)
{
	free(cpu_header);
	cpu_header = NULL;
	header_cpu_raw = false;
	headers = (struct log_records){ 0 };
}

void log_print_headers(const struct log_records *records)
{
	(void)pthread_mutex_lock(&header_lock);
	headers = *records;
	(void)pthread_mutex_unlock(&header_lock);
	write_record_headers(records, log_write_line);
}

void log_print_cpu_headers(
	const struct cpu_sample *sample,
	bool output_cpu_stats,
	bool output_cpu_raw_stats
)
{
	size_t index;
	size_t size;
	FILE *stream;
	char *header = NULL;

	if (output_cpu_stats) {
		stream = open_memstream(&header, &size);
		if (stream == NULL) {
			log_message(LOG_LEVEL_WARNING, "could not allocate CPU log header");
		} else {
			(void)fputs(cpu_header_prefix, stream);
			for (index = 0U; index < sample->count; index++) {
				const struct cpu_counter *counter = &sample->counters[index];
				const char *identifier = counter->identifier;

				(void)fputs("; ", stream);
				while (*identifier != '\0')
					(void)fputc(toupper((unsigned char)*identifier++), stream);
				(void)fputs("_USAGE", stream);
			}
			if (!log_close_stream(stream)) {
				free(header);
				header = NULL;
				log_message(LOG_LEVEL_WARNING, "could not finish CPU log header");
			}
		}
	}
	/* Built first, then swapped in, so the writer never sees a half-made header. */
	(void)pthread_mutex_lock(&header_lock);
	free(cpu_header);
	cpu_header = header;
	header_cpu_raw = output_cpu_raw_stats;
	(void)pthread_mutex_unlock(&header_lock);
	if (header != NULL)
		log_write_line(header);
	if (output_cpu_raw_stats)
		log_write_line(cpu_raw_header);
}

/* prefix, when not NULL, starts the record, followed by the formatted fields. */
static void
write_record_fields(const char *type, const char *prefix, const char *format, va_list arguments)
{
	char message[LOG_MESSAGE_SIZE];
	int prefix_length = 0;

	if (prefix != NULL)
		prefix_length = snprintf(message, sizeof(message), "%s; ", prefix);
	(void)vsnprintf(
		message + prefix_length,
		sizeof(message) - (size_t)prefix_length,
		format,
		arguments
	);
	log_write_record(type, message);
}

static void write_formatted_record(const char *type, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

static void write_formatted_record(const char *type, const char *format, ...)
{
	va_list arguments;

	if (all_logs_disabled())
		return;
	va_start(arguments, format);
	write_record_fields(type, NULL, format, arguments);
	va_end(arguments);
}

static void write_timed_record(const char *type, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/* Starts with the processing time, cake-autorate's PROC_TIME_US column. */
static void write_timed_record(const char *type, const char *format, ...)
{
	char stamp[LOG_TIMESTAMP_SIZE];
	va_list arguments;

	if (all_logs_disabled())
		return;
	va_start(arguments, format);
	write_record_fields(type, log_timestamp_text(stamp, log_realtime_us()), format, arguments);
	va_end(arguments);
}

void log_tcp_queue(const struct log_tcp_queue_record *record)
{
	write_timed_record(
		RECORD_TCP_QUEUE,
		"%d; %" PRId64 "; %d; %" PRId64,
		record->download_valid ? 1 : 0,
		record->download_queue_us,
		record->upload_valid ? 1 : 0,
		record->upload_queue_us
	);
}

void log_tcp_inject(const struct log_tcp_inject_record *record)
{
	write_timed_record(
		RECORD_TCP_INJECT,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
		record->injected,
		record->skipped,
		record->accepted,
		record->stalled
	);
}

void log_memory(const struct memory_sample *sample)
{
	write_timed_record(
		RECORD_MEMORY,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
		byte_to_kbyte(sample->rss_bytes),
		byte_to_kbyte(sample->peak_rss_bytes),
		byte_to_kbyte(sample->anonymous_bytes),
		byte_to_kbyte(sample->data_bytes)
	);
}

void log_load(const struct log_load_record *record)
{
	write_timed_record(
		RECORD_LOAD,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
		bit_to_kbit(record->download_achieved_rate_bps),
		bit_to_kbit(record->upload_achieved_rate_bps),
		bit_to_kbit(record->cake_download_rate_bps),
		bit_to_kbit(record->cake_upload_rate_bps)
	);
}

/* cake-autorate prints a ratio as a whole percent, truncated. */
static uint64_t whole_percent(uint64_t ratio_e6)
{
	return ratio_e6 / RATIO_PERCENT_E6;
}

void log_data(const struct log_data_record *record)
{
	write_timed_record(
		RECORD_DATA,
		"%" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64 ";"
		" %s; %s; %" PRIu64 ";"
		" %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRIu64 "; %" PRId64
		"; %" PRId64 "; %" PRId64 ";"
		" %" PRId64 "; %" PRIu64 "; %u; %" PRId64 "; %" PRIu64 "; %" PRIu64 "; %u; %" PRId64
		"; %" PRIu64 "; %" PRIu64 "; %s; %s; %" PRIu64 "; %" PRIu64,
		bit_to_kbit(record->download_achieved_rate_bps),
		bit_to_kbit(record->upload_achieved_rate_bps),
		whole_percent(record->download_load_ratio_e6),
		whole_percent(record->upload_load_ratio_e6),
		record->icmp_timestamp,
		record->reflector,
		record->sequence,
		record->download_owd_baseline_us,
		record->download_owd_us,
		record->download_owd_delta_ewma_us,
		record->download_owd_delta_us,
		record->download_adjust_delay_threshold_us,
		record->upload_owd_baseline_us,
		record->upload_owd_us,
		record->upload_owd_delta_ewma_us,
		record->upload_owd_delta_us,
		record->upload_adjust_delay_threshold_us,
		record->download_sum_delays,
		record->download_average_owd_delta_us,
		record->download_maximum_adjust_up_threshold_us,
		record->download_maximum_adjust_down_threshold_us,
		record->upload_sum_delays,
		record->upload_average_owd_delta_us,
		record->upload_maximum_adjust_up_threshold_us,
		record->upload_maximum_adjust_down_threshold_us,
		record->download_load_condition,
		record->upload_load_condition,
		bit_to_kbit(record->cake_download_rate_bps),
		bit_to_kbit(record->cake_upload_rate_bps)
	);
}

void log_summary(const struct log_summary_record *record)
{
	write_formatted_record(
		RECORD_SUMMARY,
		"%" PRIu64 "; %" PRIu64 "; %u; %u; %" PRId64 ";"
		" %" PRId64 "; %s; %s; %" PRIu64 "; %" PRIu64,
		bit_to_kbit(record->download_achieved_rate_bps),
		bit_to_kbit(record->upload_achieved_rate_bps),
		record->download_sum_delays,
		record->upload_sum_delays,
		record->download_average_owd_delta_us,
		record->upload_average_owd_delta_us,
		record->download_load_condition,
		record->upload_load_condition,
		bit_to_kbit(record->cake_download_rate_bps),
		bit_to_kbit(record->cake_upload_rate_bps)
	);
}

void log_reflector(const struct log_reflector_record *record)
{
	write_timed_record(
		RECORD_REFLECTOR,
		"%s; %" PRId64 "; %" PRId64 "; %" PRIu64 "; %" PRIu64 "; %" PRId64 "; %" PRId64
		"; %" PRId64 "; %" PRIu64 "; %" PRId64 "; %" PRId64 "; %" PRId64 "; %" PRIu64,
		record->reflector,
		record->minimum_sum_owd_baselines_us,
		record->sum_owd_baselines_us,
		record->sum_owd_baselines_delta_us,
		record->sum_owd_baselines_delta_threshold_us,
		record->minimum_download_delta_ewma_us,
		record->download_delta_ewma_us,
		record->download_delta_ewma_delta_us,
		record->delta_ewma_delta_threshold_us,
		record->minimum_upload_delta_ewma_us,
		record->upload_delta_ewma_us,
		record->upload_delta_ewma_delta_us,
		record->delta_ewma_delta_threshold_us
	);
}

void log_cpu(const struct cpu_sample *sample, const struct cpu_busy *usage)
{
	char *message = NULL;
	char stamp[LOG_TIMESTAMP_SIZE];
	size_t size;
	size_t index;
	FILE *stream = open_memstream(&message, &size);

	if (stream == NULL) {
		log_message(LOG_LEVEL_WARNING, "could not allocate CPU log record");
		return;
	}
	(void)fputs(log_timestamp_text(stamp, sample->timestamp_us), stream);
	for (index = 0U; index < sample->count; index++)
		(void)fprintf(
			stream,
			"; %" PRIu64,
			whole_percent(fraction_to_ratio_e6(
				usage[index].busy_ticks,
				usage[index].total_ticks
			))
		);
	if (!log_close_stream(stream))
		log_message(LOG_LEVEL_WARNING, "could not finish CPU log record");
	else
		log_write_record(RECORD_CPU, message);
	free(message);
}

void log_cpu_raw(const struct cpu_sample *sample)
{
	char stamp[LOG_TIMESTAMP_SIZE];
	size_t index;

	(void)log_timestamp_text(stamp, sample->timestamp_us);
	for (index = 0U; index < sample->count; index++) {
		const struct cpu_counter *counter = &sample->counters[index];

		write_formatted_record(
			RECORD_CPU_RAW,
			"%s; %s; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64
			"; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64 "; %" PRIu64,
			stamp,
			counter->identifier,
			counter->user_ticks,
			counter->nice_ticks,
			counter->system_ticks,
			counter->idle_ticks,
			counter->iowait_ticks,
			counter->irq_ticks,
			counter->softirq_ticks,
			counter->steal_ticks,
			counter->guest_ticks,
			counter->guest_nice_ticks
		);
	}
}

void log_shaper(const char *interface, uint64_t rate_bps)
{
	write_formatted_record(
		RECORD_SHAPER,
		"tc qdisc change root dev %s cake bandwidth %" PRIu64 "Kbit",
		interface,
		bit_to_kbit(rate_bps)
	);
}
