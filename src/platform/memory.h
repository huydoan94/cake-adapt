#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* The daemon's own memory use from /proc/self/status, in kilobytes. */
struct memory_sample {
	/* Resident now (VmRSS), and its highest value since start (VmHWM). */
	uint64_t rss_kilobytes;
	uint64_t peak_rss_kilobytes;
	/* Resident heap and other private memory (RssAnon); a leak grows it. */
	uint64_t anonymous_kilobytes;
	/* Data and heap mappings, resident or not (VmData). */
	uint64_t data_kilobytes;
};

int memory_read(const char *path, struct memory_sample *sample, char *error, size_t error_size);

#endif
