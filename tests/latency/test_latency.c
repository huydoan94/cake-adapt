#define _POSIX_C_SOURCE 200809L

#include "latency/latency.h"
#include "common/constants.h"

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* access() is wrapped at link time so the backend check does not depend on the host. */
static bool fake_access;
static int access_result;
static int access_errno;
static char accessed_path[64];

int __real_access(const char *path, int mode);
int __wrap_access(const char *path, int mode);

int __wrap_access(const char *path, int mode)
{
	if (!fake_access)
		return __real_access(path, mode);
	assert(mode == X_OK);
	(void)snprintf(accessed_path, sizeof(accessed_path), "%s", path);
	errno = access_errno;
	return access_result;
}

/* Linux wait status encodings, as reported by waitpid() and uloop. */
#define EXIT_STATUS(code) ((code) << 8)
#define SIGNAL_STATUS(signal) (signal)

static void test_initial_state_is_closed(void)
{
	struct latency latency;

	latency_init(&latency);

	assert(latency.children[0].output_descriptor == -1);
	assert(latency.children[0].process_identifier == -1);
	assert(latency_child_count(&latency) == 0U);
	assert(latency_child_descriptor(&latency, 0U) == -1);
	assert(latency_child_process(&latency, 0U) == -1);
	assert(!latency_is_open(&latency));
	assert(!latency_stopping(&latency));
}

static void test_output_is_split_into_lines(void)
{
	const char output[] = "[123.456000] 1.1.1.1 : [1], 64 bytes, 2.50 ms\r\npartial";
	char line[LATENCY_OUTPUT_SIZE];
	char long_output[LATENCY_OUTPUT_SIZE + 1U];
	size_t consumed = 0U;

	assert(latency_next_line(output, sizeof(output) - 1U, line, &consumed) ==
	       LATENCY_LINE_READY);
	assert(strcmp(line, "[123.456000] 1.1.1.1 : [1], 64 bytes, 2.50 ms") == 0);
	assert(consumed == strlen(line) + 2U);
	assert(latency_next_line(output + consumed, sizeof(output) - 1U - consumed, line,
				 &consumed) == LATENCY_LINE_INCOMPLETE);
	assert(latency_next_line("", 0U, line, &consumed) == LATENCY_LINE_INCOMPLETE);

	/* The newline must fall within LATENCY_OUTPUT_SIZE bytes. */
	memset(long_output, 'x', sizeof(long_output));
	long_output[LATENCY_OUTPUT_SIZE - 1U] = '\n';
	assert(latency_next_line(long_output, sizeof(long_output), line, &consumed) ==
	       LATENCY_LINE_READY);
	assert(consumed == LATENCY_OUTPUT_SIZE);
	assert(strlen(line) == LATENCY_OUTPUT_SIZE - 1U);
	long_output[LATENCY_OUTPUT_SIZE - 1U] = 'x';
	long_output[LATENCY_OUTPUT_SIZE] = '\n';
	assert(latency_next_line(long_output, sizeof(long_output), line, &consumed) ==
	       LATENCY_LINE_TOO_LONG);
	assert(latency_next_line(long_output, LATENCY_OUTPUT_SIZE - 1U, line, &consumed) ==
	       LATENCY_LINE_INCOMPLETE);
}

static void test_fping_lines_are_handled(void)
{
	struct latency latency;
	struct latency_sample sample;
	char error[256] = "";

	latency_init(&latency);
	latency.child_count = 1U;
	assert(latency_handle_line(&latency, 0U, "[123.456000] 1.1.1.1 : [1], 64 bytes, 2.50 ms",
				   &sample, error, sizeof(error)) == LATENCY_PROBE_SUCCESS);
	assert(sample.sequence == 1U);
	assert(sample.download_owd_microseconds == 1250U);
	assert(sample.upload_owd_microseconds == 1250U);
	assert(latency_handle_line(&latency, 0U, "[123.756000] 1.1.1.1 : [2], timed out", &sample,
				   error, sizeof(error)) == LATENCY_PROBE_TIMEOUT);
	assert(sample.sequence == 2U);
	assert(latency_handle_line(&latency, 0U, "bad", &sample, error, sizeof(error)) ==
	       LATENCY_PROBE_ERROR);
	assert(strstr(error, "unexpected fping output") != NULL);

	/* An fping-ts session reads one-way delays from the ICMP timestamps. */
	latency.backend = LATENCY_BACKEND_FPING_TS;
	assert(latency_handle_line(
		       &latency, 0U,
		       "[123.456000] 1.1.1.1 : [3], 20 bytes, 2.50 ms (2.50 avg, 0% loss),"
		       " timestamps: Originate=1000 Receive=1004 Transmit=1004 Localreceive=1006",
		       &sample, error, sizeof(error)) == LATENCY_PROBE_SUCCESS);
	assert(sample.download_owd_microseconds == 2000);
	assert(sample.upload_owd_microseconds == 4000);
	assert(sample.timestamp_rollover_sensitive);
	assert(latency_handle_line(&latency, 0U, "[123.456000] 1.1.1.1 : [4], 64 bytes, 2.50 ms",
				   &sample, error, sizeof(error)) == LATENCY_PROBE_ERROR);
}

static void test_exit_status_is_reported(void)
{
	struct latency latency;
	char error[256] = "";

	latency_init(&latency);
	latency.active = true;
	latency.child_count = 1U;
	/* No process is signalled: exits are only reported here. */
	latency.children[0].process_identifier = 424242;
	assert(latency_child_exited(&latency, 0U, EXIT_STATUS(3), error, sizeof(error)) ==
	       LATENCY_PROBE_ERROR);
	assert(strcmp(error, "fping exited with status 3") == 0);
	assert(latency_child_process(&latency, 0U) == -1);

	latency.children[0].process_identifier = 424242;
	assert(latency_child_exited(&latency, 0U, SIGNAL_STATUS(SIGKILL), error, sizeof(error)) ==
	       LATENCY_PROBE_ERROR);
	assert(strcmp(error, "fping terminated by signal 9") == 0);
}

static void test_invalid_target_is_rejected_before_starting_fping(void)
{
	struct latency latency;
	const char *targets[] = { "not an endpoint" };
	char error[256] = "";

	latency_init(&latency);

	assert(latency_open(&latency, "lo", targets, 1U, 1000000U, "", "", false, error,
			    sizeof(error)) != 0);
	assert(latency.children[0].output_descriptor == -1);
	assert(latency.children[0].process_identifier == -1);
	assert(strlen(error) > 0U);
}

static void test_empty_target_list_is_rejected(void)
{
	struct latency latency;
	char error[256] = "";

	latency_init(&latency);
	assert(latency_open(&latency, "lo", NULL, 0U, 1000000U, "", "", false, error,
			    sizeof(error)) != 0);
	assert(strstr(error, "at least one target") != NULL);
	assert(!latency_is_open(&latency));
}

static void test_sub_millisecond_response_spacing_is_rejected(void)
{
	struct latency latency;
	const char *targets[] = { "1.1.1.1", "9.9.9.9" };
	char error[256] = "";

	latency_init(&latency);
	assert(latency_open(&latency, "lo", targets, 2U, 1999U, "", "", false, error,
			    sizeof(error)) != 0);
	assert(strstr(error, "at least 1 ms per target") != NULL);
	assert(!latency_is_open(&latency));
}

static void test_close_is_idempotent(void)
{
	struct latency latency;

	latency_init(&latency);
	latency_close(&latency);
	latency_close(&latency);

	assert(latency.children[0].output_descriptor == -1);
	assert(latency.children[0].process_identifier == -1);
}

static void test_close_releases_every_owned_descriptor(void)
{
	struct latency latency;
	int first[2];
	int second[2];

	latency_init(&latency);
	assert(pipe(first) == 0);
	assert(pipe(second) == 0);
	latency.children[0].output_descriptor = first[0];
	latency.children[1].output_descriptor = second[0];
	latency.child_count = 2U;
	latency_close(&latency);
	assert(fcntl(first[0], F_GETFD) == -1);
	assert(errno == EBADF);
	assert(fcntl(second[0], F_GETFD) == -1);
	assert(errno == EBADF);
	assert(latency_child_count(&latency) == 0U);
	assert(latency_child_descriptor(&latency, 0U) == -1);
	assert(latency_child_descriptor(&latency, CONFIG_MAX_REFLECTORS) == -1);
	assert(close(first[1]) == 0);
	assert(close(second[1]) == 0);
}

static void test_irtt_lines_ignore_non_samples(void)
{
	struct latency latency;
	struct latency_sample sample;
	char error[256] = "";

	latency_init(&latency);
	latency.backend = LATENCY_BACKEND_IRTT;
	latency.active = true;
	latency.child_count = 1U;
	latency.children[0].target = "9.9.9.9";
	assert(latency_handle_line(&latency, 0U, "IRTT client", &sample, error, sizeof(error)) ==
	       LATENCY_PROBE_PENDING);
	assert(latency_handle_line(&latency, 0U, "seq=7 rtt=3ms rd=1ms sd=2ms", &sample, error,
				   sizeof(error)) == LATENCY_PROBE_SUCCESS);
	assert(strcmp(sample.target, "9.9.9.9") == 0);
	assert(sample.sequence == 7U);
	assert(sample.download_owd_microseconds == 1000);
	assert(sample.upload_owd_microseconds == 2000);
	assert(sample.timestamp_microseconds > 0U);
	/* Like cake-autorate's gawk wrapper: receive time in whole microseconds. */
	assert(strtoull(sample.timestamp_text, NULL, 10) == sample.timestamp_microseconds);
}

/* The test stands in for uloop: it reaps the child and reports the exit. */
static void reap_stopped_child(struct latency *latency, size_t child_index,
			       pid_t process_identifier)
{
	char error[256] = "";
	int status;

	assert(waitpid(process_identifier, &status, 0) == process_identifier);
	assert(latency_child_exited(latency, child_index, status, error, sizeof(error)) ==
	       LATENCY_PROBE_STOPPED);
	assert(error[0] == '\0');
}

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now;

	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void open_long_running_child(struct latency *latency, const char *script)
{
	const char *targets[] = { "127.0.0.1" };
	char prefix[128];
	char error[256] = "";

	/* The fping arguments become the shell's positional parameters. */
	(void)snprintf(prefix, sizeof(prefix), "/bin/sh -c '%s'", script);
	latency_init(latency);
	assert(latency_open(latency, "lo", targets, 1U, 1000000U, "", prefix, false, error,
			    sizeof(error)) == 0);
	assert(latency_child_process(latency, 0U) > 0);
}

static void test_close_signals_without_waiting(void)
{
	struct latency latency;
	pid_t process_identifier;
	uint64_t started;

	open_long_running_child(&latency, "sleep 30");
	process_identifier = latency_child_process(&latency, 0U);
	started = monotonic_milliseconds();
	latency_close(&latency);
	assert(monotonic_milliseconds() - started < 100U);
	assert(!latency_is_open(&latency));
	assert(latency_stopping(&latency));
	assert(latency_child_descriptor(&latency, 0U) == -1);
	/* Closing again must not signal the same child twice. */
	latency_close(&latency);
	reap_stopped_child(&latency, 0U, process_identifier);
	assert(!latency_stopping(&latency));
	assert(latency_child_process(&latency, 0U) == -1);
}

static void test_ignored_sigterm_escalates_to_sigkill(void)
{
	struct latency latency;
	const struct timespec settle = { .tv_nsec = 100000000L };
	pid_t process_identifier;
	char error[256] = "";
	unsigned int attempts;
	int status;

	open_long_running_child(&latency, "trap \"\" TERM; sleep 30; sleep 30");
	process_identifier = latency_child_process(&latency, 0U);
	(void)nanosleep(&settle, NULL);
	latency_close(&latency);
	(void)nanosleep(&settle, NULL);
	assert(waitpid(process_identifier, &status, WNOHANG) == 0);
	latency_kill_stopping(&latency);
	assert(waitpid(process_identifier, &status, 0) == process_identifier);
	assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
	assert(latency_child_exited(&latency, 0U, status, error, sizeof(error)) ==
	       LATENCY_PROBE_STOPPED);
	/* The whole group was killed; the orphaned sleep is reaped by init shortly after. */
	for (attempts = 0U; kill(-process_identifier, 0) == 0 && attempts < 200U; attempts++) {
		const struct timespec pause = { .tv_nsec = 10000000L };

		(void)nanosleep(&pause, NULL);
	}
	assert(kill(-process_identifier, 0) == -1 && errno == ESRCH);
	assert(!latency_stopping(&latency));
}

static void test_stop_now_reaps_before_returning(void)
{
	struct latency latency;
	pid_t process_identifier;

	open_long_running_child(&latency, "sleep 30");
	process_identifier = latency_child_process(&latency, 0U);
	latency_stop_now(&latency);
	assert(waitpid(process_identifier, NULL, WNOHANG) == -1 && errno == ECHILD);
	assert(!latency_stopping(&latency));
	assert(latency_child_process(&latency, 0U) == -1);
}

static void test_pinger_arguments_reject_command_substitution(void)
{
	struct latency latency;
	const char *targets[] = { "1.1.1.1" };
	char error[256] = "";

	latency_init(&latency);
	assert(latency_open(&latency, "lo", targets, 1U, 1000000U, "$(id)", "", false, error,
			    sizeof(error)) != 0);
	assert(strstr(error, "ping_extra_args") != NULL);
	assert(!latency_is_open(&latency));
	assert(latency_open(&latency, "lo", targets, 1U, 1000000U, "", "'unterminated", false,
			    error, sizeof(error)) != 0);
	assert(strstr(error, "ping_prefix_string") != NULL);
	assert(!latency_is_open(&latency));
}

static size_t open_descriptor_count(void)
{
	DIR *directory = opendir("/proc/self/fd");
	size_t count = 0U;

	assert(directory != NULL);
	while (readdir(directory) != NULL)
		count++;
	assert(closedir(directory) == 0);
	return count;
}

static void test_failed_spawn_closes_pipe(void)
{
	struct latency latency;
	const char *targets[] = { "127.0.0.1" };
	char error[256];
	size_t descriptors = open_descriptor_count();

	latency_init(&latency);
	assert(latency_open(&latency, "lo", targets, 1U, 1000000U, "",
			    "/nonexistent-cake-adapt-test/fping", false, error,
			    sizeof(error)) != 0);
	assert(strstr(error, "could not start fping") != NULL);
	assert(!latency_is_open(&latency));
	assert(open_descriptor_count() == descriptors);
}

/* The printf prefix echoes the fping command line, one argument per line. */
static void check_fping_arguments(bool icmp_timestamps, const char *expected)
{
	struct latency latency;
	pid_t process_identifier;
	const char *targets[] = { "1.1.1.1", "::1" };
	char error[256] = "";
	char output[1024];
	size_t length = 0U;
	struct pollfd descriptor;

	latency_init(&latency);
	assert(latency_open(&latency, "lo", targets, 2U, 300000U, "-I 'lo2' -k 768",
			    "/usr/bin/printf '%s\\n'", icmp_timestamps, error, sizeof(error)) == 0);
	assert(latency.backend ==
	       (icmp_timestamps ? LATENCY_BACKEND_FPING_TS : LATENCY_BACKEND_FPING));
	descriptor =
		(struct pollfd){ .fd = latency.children[0].output_descriptor, .events = POLLIN };
	for (;;) {
		ssize_t bytes;

		assert(poll(&descriptor, 1U, 1000) > 0);
		assert(length < sizeof(output) - 1U);
		bytes = read(latency.children[0].output_descriptor, output + length,
			     sizeof(output) - 1U - length);
		assert(bytes >= 0);
		if (bytes == 0)
			break;
		length += (size_t)bytes;
	}
	output[length] = '\0';
	assert(strcmp(output, expected) == 0);
	process_identifier = latency_child_process(&latency, 0U);
	latency_close(&latency);
	assert(!latency_is_open(&latency));
	assert(latency_child_count(&latency) == 0U);
	reap_stopped_child(&latency, 0U, process_identifier);
}

static void test_prefix_and_extra_args_reach_owned_process(void)
{
	check_fping_arguments(false,
			      "/usr/bin/fping\n-I\nlo2\n-k\n768\n--timestamp\n--loop\n"
			      "--period\n300\n--interval\n150\n--timeout\n10000\n1.1.1.1\n::1\n");
	/* Like cake-autorate's fping-ts, --icmp-timestamp follows --timeout. */
	check_fping_arguments(true,
			      "/usr/bin/fping\n-I\nlo2\n-k\n768\n--timestamp\n--loop\n"
			      "--period\n300\n--interval\n150\n--timeout\n10000\n--icmp-timestamp\n"
			      "1.1.1.1\n::1\n");
	assert(target_is_valid("::1"));
	assert(target_is_valid("2001:4860:4860::8888"));
}

static void test_irtt_children_start_in_separate_slots(void)
{
	struct latency latency;
	pid_t first;
	pid_t second;
	int status;
	const char *targets[] = { "1.1.1.1", "2001:db8::1" };
	char error[256] = "";
	char output[1024];
	size_t length = 0U;
	struct pollfd descriptor;

	latency_init(&latency);
	assert(latency_open_irtt(&latency, targets, 2U, 300U * MILLISECOND, 10U, "--fill=rand",
				 "/usr/bin/printf '%s\\n'", 1000U, error, sizeof(error)) == 0);
	assert(latency_is_open(&latency));
	assert(latency_child_count(&latency) == 2U);
	assert(latency_child_descriptor(&latency, 0U) == -1);
	assert(latency_child_descriptor(&latency, 1U) == -1);
	assert(latency_irtt_start_pending(&latency));
	assert(latency_start_irtt_children(&latency, 999U, error, sizeof(error)) == 0);
	assert(latency_child_descriptor(&latency, 0U) == -1);
	assert(latency_child_descriptor(&latency, 1U) == -1);
	assert(latency_start_irtt_children(&latency, 1000U, error, sizeof(error)) == 0);
	assert(latency_child_descriptor(&latency, 0U) >= 0);
	assert(latency_child_descriptor(&latency, 1U) == -1);
	assert(latency_irtt_next_start_microseconds(&latency) == 151000U);

	descriptor =
		(struct pollfd){ .fd = latency_child_descriptor(&latency, 0U), .events = POLLIN };
	for (;;) {
		ssize_t bytes;

		assert(poll(&descriptor, 1U, 1000) > 0);
		bytes = read(descriptor.fd, output + length, sizeof(output) - 1U - length);
		assert(bytes >= 0);
		if (bytes == 0)
			break;
		length += (size_t)bytes;
	}
	output[length] = '\0';
	assert(strcmp(output, "/usr/bin/irtt\nclient\n--fill=rand\n-i\n0.300000s\n"
			      "-d\n10m\n1.1.1.1\n") == 0);

	assert(latency_start_irtt_children(&latency, 151000U, error, sizeof(error)) == 0);
	assert(latency_child_descriptor(&latency, 1U) >= 0);
	assert(!latency_irtt_start_pending(&latency));
	/* A finished session is reaped and restarted without stopping the others. */
	first = latency_child_process(&latency, 0U);
	assert(waitpid(first, &status, 0) == first);
	assert(latency_child_exited(&latency, 0U, status, error, sizeof(error)) ==
	       LATENCY_PROBE_RESTART);
	assert(strstr(error, "irtt exited with status 0") != NULL);
	assert(latency_child_descriptor(&latency, 0U) == -1);
	assert(latency_child_process(&latency, 0U) == -1);
	assert(latency_child_descriptor(&latency, 1U) >= 0);
	assert(latency_irtt_start_pending(&latency));
	second = latency_child_process(&latency, 1U);
	latency_close(&latency);
	assert(!latency_is_open(&latency));
	reap_stopped_child(&latency, 1U, second);
	assert(!latency_stopping(&latency));
}

static void test_backend_executable_must_be_available(void)
{
	char error[256] = "";

	assert(strcmp(latency_backend_executable("fping"), "/usr/bin/fping") == 0);
	assert(strcmp(latency_backend_executable("fping-ts"), "/usr/bin/fping") == 0);
	assert(strcmp(latency_backend_executable("irtt"), "/usr/bin/irtt") == 0);
	assert(latency_backend_executable("ping") == NULL);

	fake_access = true;
	access_result = 0;
	access_errno = 0;
	accessed_path[0] = '\0';
	assert(latency_check_backend("fping", error, sizeof(error)) == 0);
	assert(strcmp(accessed_path, "/usr/bin/fping") == 0);

	access_result = -1;
	access_errno = ENOENT;
	assert(latency_check_backend("irtt", error, sizeof(error)) == -1);
	assert(strcmp(accessed_path, "/usr/bin/irtt") == 0);
	assert(strcmp(error,
		      "ping binary /usr/bin/irtt for pinger_method 'irtt' is not available: No such file or directory") ==
	       0);

	accessed_path[0] = '\0';
	assert(latency_check_backend("ping", error, sizeof(error)) == -1);
	assert(accessed_path[0] == '\0');
	assert(strcmp(error, "unknown pinger_method 'ping'") == 0);
	fake_access = false;
}

int main(void)
{
	test_initial_state_is_closed();
	test_output_is_split_into_lines();
	test_fping_lines_are_handled();
	test_exit_status_is_reported();
	test_invalid_target_is_rejected_before_starting_fping();
	test_empty_target_list_is_rejected();
	test_sub_millisecond_response_spacing_is_rejected();
	test_close_is_idempotent();
	test_close_releases_every_owned_descriptor();
	test_irtt_lines_ignore_non_samples();
	test_close_signals_without_waiting();
	test_ignored_sigterm_escalates_to_sigkill();
	test_stop_now_reaps_before_returning();
	test_pinger_arguments_reject_command_substitution();
	test_failed_spawn_closes_pipe();
	test_prefix_and_extra_args_reach_owned_process();
	test_irtt_children_start_in_separate_slots();
	test_backend_executable_must_be_available();

	(void)puts("latency tests passed");
	return 0;
}
