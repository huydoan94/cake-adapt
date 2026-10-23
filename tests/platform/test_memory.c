#define _POSIX_C_SOURCE 200809L

#include "platform/memory.h"
#include "constants/units.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_status(char *path, const char *contents)
{
	int descriptor = mkstemp(path);
	FILE *file;

	assert(descriptor >= 0);
	file = fdopen(descriptor, "w");
	assert(file != NULL);
	assert(fputs(contents, file) >= 0);
	assert(fclose(file) == 0);
}

static void test_status_parsing(void)
{
	char path[] = "/tmp/cake-adapt-memory-test-XXXXXX";
	char error[256] = "";
	struct memory_sample sample = { 0 };

	write_status(
		path,
		"Name:\tcake-adapt\n"
		"VmPeak:\t    3000 kB\n"
		"VmHWM:\t    2200 kB\n"
		"VmRSS:\t    2136 kB\n"
		"RssAnon:\t     412 kB\n"
		"RssFile:\t    1724 kB\n"
		"VmData:\t     900 kB\n"
		"Threads:\t1\n"
	);
	assert(memory_read(path, &sample, error, sizeof(error)) == 0);
	assert(sample.rss_bytes == 2136U * KILOBYTE);
	assert(sample.peak_rss_bytes == 2200U * KILOBYTE);
	assert(sample.anonymous_bytes == 412U * KILOBYTE);
	assert(sample.data_bytes == 900U * KILOBYTE);
	assert(unlink(path) == 0);
}

static void test_missing_field_and_file(void)
{
	char path[] = "/tmp/cake-adapt-memory-test-XXXXXX";
	char error[256] = "";
	struct memory_sample sample;

	/* No RssAnon, as on kernels before 4.5. */
	write_status(path, "VmHWM:\t 2200 kB\nVmRSS:\t 2136 kB\nVmData:\t 900 kB\n");
	assert(memory_read(path, &sample, error, sizeof(error)) != 0);
	assert(strstr(error, "missing") != NULL);
	assert(unlink(path) == 0);
	assert(memory_read(path, &sample, error, sizeof(error)) != 0);
	assert(strstr(error, "could not open") != NULL);
}

/*
 * The peak is the highest VmHWM or VmRSS seen, never lower than an earlier
 * one: a lazily updated VmHWM below the current RSS, then dropping, as seen on
 * a router (2164 then 2148 kB).
 */
static void test_peak_never_drops(void)
{
	uint64_t peak_bytes = 0U;
	struct memory_sample sample = {
		.rss_bytes = 2164U * KILOBYTE,
		.peak_rss_bytes = 2100U * KILOBYTE,
	};

	memory_track_peak(&sample, &peak_bytes);
	assert(sample.peak_rss_bytes == 2164U * KILOBYTE && peak_bytes == sample.peak_rss_bytes);
	sample.rss_bytes = 2140U * KILOBYTE;
	sample.peak_rss_bytes = 2148U * KILOBYTE;
	memory_track_peak(&sample, &peak_bytes);
	assert(sample.peak_rss_bytes == 2164U * KILOBYTE);
	/* A VmHWM above every sampled RSS, from growth between samples, is kept. */
	sample.peak_rss_bytes = 2400U * KILOBYTE;
	memory_track_peak(&sample, &peak_bytes);
	assert(sample.peak_rss_bytes == 2400U * KILOBYTE && peak_bytes == 2400U * KILOBYTE);
}

/* The host's own status file has every field the daemon reads. */
static void test_own_status(void)
{
	char error[256] = "";
	struct memory_sample sample;

	assert(memory_read("/proc/self/status", &sample, error, sizeof(error)) == 0);
	assert(sample.rss_bytes > 0U && sample.peak_rss_bytes >= sample.rss_bytes);
}

int main(void)
{
	test_status_parsing();
	test_missing_field_and_file();
	test_peak_never_drops();
	test_own_status();
	puts("memory tests passed");
	return 0;
}
