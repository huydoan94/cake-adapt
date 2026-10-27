#ifndef PLATFORM_MEMORY_H_INCLUDED
#define PLATFORM_MEMORY_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

/* The daemon's own memory use from /proc/self/status, in bytes. */
struct memory_sample {
	/*
	 * Resident now (VmRSS), and its highest value since start: VmHWM as read,
	 * or the highest seen once memory_track_peak() has run.
	 */
	uint64_t rss_bytes;
	uint64_t peak_rss_bytes;
	/* Resident heap and other private memory (RssAnon); a leak grows it. */
	uint64_t anonymous_bytes;
	/* Data and heap mappings, resident or not (VmData). */
	uint64_t data_bytes;
};

int memory_read(const char *path, struct memory_sample *sample, char *error, size_t error_size);

/*
 * Raises *peak_bytes to the sample's VmHWM or VmRSS, whichever is higher,
 * and stores it as the sample's peak. The kernel updates VmHWM lazily, so on
 * its own it can read below an earlier sample's RSS, and even drop.
 */
void memory_track_peak(struct memory_sample *sample, uint64_t *peak_bytes);

#endif
