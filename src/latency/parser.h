#ifndef LATENCY_PARSER_H_INCLUDED
#define LATENCY_PARSER_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#define LATENCY_TARGET_SIZE 256U
/* Room for the pinger's own timestamp token, logged verbatim like cake-autorate. */
#define LATENCY_TIMESTAMP_TEXT_SIZE 48U

struct latency_sample {
	char target[LATENCY_TARGET_SIZE];
	int64_t download_owd_microseconds;
	int64_t upload_owd_microseconds;
	uint64_t timestamp_microseconds;
	/* fping's "[seconds.fraction]" token, or IRTT's receive time in whole microseconds. */
	char timestamp_text[LATENCY_TIMESTAMP_TEXT_SIZE];
	bool timestamp_rollover_sensitive;
	uint64_t sequence;
};

enum latency_fping_line_result {
	LATENCY_FPING_LINE_SAMPLE,
	LATENCY_FPING_LINE_TIMEOUT,
	LATENCY_FPING_LINE_INVALID
};

enum latency_fping_line_result parse_fping_line(const char *line, struct latency_sample *sample);

/* An fping --icmp-timestamp reply, which carries separate one-way delays. */
enum latency_fping_line_result parse_fping_timestamp_line(const char *line,
							  struct latency_sample *sample);

bool parse_irtt_line(const char *line, const char *target, uint64_t timestamp_microseconds,
		     struct latency_sample *sample);

#endif
