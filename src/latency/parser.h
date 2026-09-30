#ifndef LATENCY_PARSER_H_INCLUDED
#define LATENCY_PARSER_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#define LATENCY_TARGET_SIZE 256U

struct latency_sample {
    char target[LATENCY_TARGET_SIZE];
    int64_t download_owd_microseconds;
    int64_t upload_owd_microseconds;
    uint64_t timestamp_microseconds;
    bool timestamp_rollover_sensitive;
    uint64_t sequence;
};

enum latency_fping_line_result {
    LATENCY_FPING_LINE_SAMPLE,
    LATENCY_FPING_LINE_TIMEOUT,
    LATENCY_FPING_LINE_INVALID
};

enum latency_fping_line_result parse_fping_line(
    const char *line,
    struct latency_sample *sample
);

bool parse_irtt_line(
    const char *line,
    const char *target,
    uint64_t timestamp_microseconds,
    struct latency_sample *sample
);

#endif
