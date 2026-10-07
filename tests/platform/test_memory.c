#define _POSIX_C_SOURCE 200809L

#include "platform/memory.h"

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
	assert(sample.rss_kilobytes == 2136U);
	assert(sample.peak_rss_kilobytes == 2200U);
	assert(sample.anonymous_kilobytes == 412U);
	assert(sample.data_kilobytes == 900U);
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

/* The host's own status file has every field the daemon reads. */
static void test_own_status(void)
{
	char error[256] = "";
	struct memory_sample sample;

	assert(memory_read("/proc/self/status", &sample, error, sizeof(error)) == 0);
	assert(sample.rss_kilobytes > 0U && sample.peak_rss_kilobytes >= sample.rss_kilobytes);
}

int main(void)
{
	test_status_parsing();
	test_missing_field_and_file();
	test_own_status();
	puts("memory tests passed");
	return 0;
}
