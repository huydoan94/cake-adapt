/* SPDX-License-Identifier: MIT */
/* Read-only sampler: existing daemon program/maps, no second filter attachment. */
#define _GNU_SOURCE
#include "common/constants.h"
#include "common/utils.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <inttypes.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>

#define PIN_MODE "--pin"
#define LIMIT_MODE "--limit"
#define COUNTERS_NAME "counters"
#define FILE_MODE "r"
#define FIELD_SEPARATOR " "
#define LINE_BYTES 4096U
#define MAX_MAPS 16U

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t expired;
static volatile sig_atomic_t limited_pid;

static void limit_signal(int signal_number)
{
	(void)signal_number;
	(void)kill((pid_t)limited_pid, expired ? SIGKILL : SIGHUP);
	expired = 1;
	(void)alarm(10U);
}

static void stop_sample(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static void require(int valid, const char *operation)
{
	if (!valid) {
		perror(operation);
		exit(EXIT_FAILURE);
	}
}

/* Independent VM-side deadline; leave ten seconds for the shell's cleanup. */
static int limit_command(unsigned int seconds, char **arguments)
{
	sigset_t blocked, original;
	pid_t child;
	int status;

	(void)sigemptyset(&blocked);
	(void)sigaddset(&blocked, SIGHUP);
	(void)sigaddset(&blocked, SIGTERM);
	(void)sigaddset(&blocked, SIGINT);
	(void)sigaddset(&blocked, SIGALRM);
	require(sigprocmask(SIG_BLOCK, &blocked, &original) == 0, "deadline block signals");
	child = fork();
	require(child >= 0, "deadline fork");
	if (child == 0) {
		require(sigprocmask(SIG_SETMASK, &original, NULL) == 0, "deadline child mask");
		execvp(arguments[0], arguments);
		perror("deadline execute");
		_exit(127);
	}
	limited_pid = (sig_atomic_t)child;
	require(signal(SIGALRM, limit_signal) != SIG_ERR &&
			signal(SIGHUP, limit_signal) != SIG_ERR &&
			signal(SIGTERM, limit_signal) != SIG_ERR &&
			signal(SIGINT, limit_signal) != SIG_ERR,
		"deadline handlers");
	(void)alarm(seconds);
	require(sigprocmask(SIG_SETMASK, &original, NULL) == 0, "deadline parent mask");
	while (waitpid(child, &status, 0) < 0)
		require(errno == EINTR, "deadline wait");
	(void)alarm(0U);
	if (expired)
		return 124;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static uint64_t clock_us(clockid_t clock)
{
	struct timespec value;
	require(clock_gettime(clock, &value) == 0, "clock");
	return timespec_microseconds(&value);
}

static void descriptor_info(int descriptor)
{
	char path[80], line[LINE_BYTES];
	FILE *file;

	(void)snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", descriptor);
	file = fopen(path, FILE_MODE);
	require(file != NULL, "fdinfo");
	while (fgets(line, sizeof(line), file))
		printf("# %s", line);
	(void)fclose(file);
}

static uint64_t daemon_ticks(unsigned int pid, unsigned long *rss)
{
	char path[80], line[LINE_BYTES], *token, *state;
	FILE *file;
	uint64_t ticks = 0U;
	unsigned int field;

	(void)snprintf(path, sizeof(path), "/proc/%u/stat", pid);
	file = fopen(path, FILE_MODE);
	require(file != NULL && fgets(line, sizeof(line), file) != NULL, "daemon stat");
	(void)fclose(file);
	state = strrchr(line, ')');
	require(state != NULL, "stat comm");
	token = strtok(state + 1, FIELD_SEPARATOR);
	for (field = 3U; field <= 24U; field++) {
		require(token != NULL, "stat fields");
		if (field == 14U || field == 15U)
			ticks += strtoull(token, NULL, 10);
		if (field == 24U)
			*rss = strtoul(token, NULL, 10);
		token = strtok(NULL, FIELD_SEPARATOR);
	}
	return ticks;
}

int main(int argc, char **argv)
{
	struct bpf_prog_info program = { 0 };
	uint32_t map_ids[MAX_MAPS], size, key = 0U;
	unsigned int pid, program_id, seconds, index, words = 0U;
	int stats_fd, program_fd = -1, counters_fd = -1;
	int cpus = libbpf_num_possible_cpus();
	uint64_t *values = NULL, end;

	if (argc >= 4 && strcmp(argv[1], LIMIT_MODE) == 0) {
		seconds = (unsigned int)strtoul(argv[2], NULL, 10);
		require(seconds > 0U && seconds <= 320U, "bounded deadline");
		return limit_command(seconds, argv + 3);
	}
	if (argc >= 3 && strcmp(argv[1], PIN_MODE) == 0) {
		cpu_set_t affinity;

		CPU_ZERO(&affinity);
		CPU_SET(0, &affinity);
		require(sched_setaffinity(0, sizeof(affinity), &affinity) == 0, "CPU affinity");
		execvp(argv[2], argv + 2);
		require(0, "execute pinned command");
	}
	require(argc == 4, "usage: sample PID PROGRAM_ID_OR_ZERO SECONDS");
	pid = (unsigned int)strtoul(argv[1], NULL, 10);
	program_id = (unsigned int)strtoul(argv[2], NULL, 10);
	seconds = (unsigned int)strtoul(argv[3], NULL, 10);
	require(seconds > 0U && seconds <= 120U && cpus > 0, "bounded duration/CPUs");
	(void)signal(SIGTERM, stop_sample);
	stats_fd = bpf_enable_stats(BPF_STATS_RUN_TIME);
	require(stats_fd >= 0, "BPF stats reference");
	printf("# hz=%ld page_bytes=%ld possible_cpus=%d\n",
	       sysconf(_SC_CLK_TCK),
	       sysconf(_SC_PAGESIZE),
	       cpus);
	if (program_id) {
		program_fd = bpf_prog_get_fd_by_id(program_id);
		require(program_fd >= 0, "program by ID");
		program.nr_map_ids = MAX_MAPS;
		program.map_ids = (uint64_t)(uintptr_t)map_ids;
		size = sizeof(program);
		require(bpf_prog_get_info_by_fd(program_fd, &program, &size) == 0 &&
				program.jited_prog_len != 0U && program.nr_map_ids <= MAX_MAPS,
			"JIT and maps");
		printf("# program_id=%u jit_bytes=%u xlated_bytes=%u\n",
		       program_id,
		       program.jited_prog_len,
		       program.xlated_prog_len);
		descriptor_info(program_fd);
		for (index = 0U; index < program.nr_map_ids; index++) {
			struct bpf_map_info map = { 0 };
			int descriptor = bpf_map_get_fd_by_id(map_ids[index]);

			size = sizeof(map);
			require(descriptor >= 0 &&
					bpf_map_get_info_by_fd(descriptor, &map, &size) == 0,
				"map info");
			printf("# map=%s type=%u key=%u value=%u max_entries=%u\n",
			       map.name,
			       map.type,
			       map.key_size,
			       map.value_size,
			       map.max_entries);
			descriptor_info(descriptor);
			if (strcmp(map.name, COUNTERS_NAME) == 0) {
				require(map.type == BPF_MAP_TYPE_PERCPU_ARRAY &&
						map.key_size == 4U && map.max_entries == 1U &&
						(map.value_size == 24U || map.value_size == 32U),
					"counter ABI");
				words = map.value_size / sizeof(uint64_t);
				counters_fd = descriptor;
			} else
				(void)close(descriptor);
		}
		require(counters_fd >= 0, "counters map");
		values = calloc((size_t)cpus * words, sizeof(*values));
		require(values != NULL, "counter allocation");
	}
	puts(
		"realtime_us monotonic_us daemon_ticks rss_pages runs runtime_ns ring_full ack_bytes upload_bytes unaccounted host_user host_nice host_system host_idle host_iowait host_irq host_softirq host_steal"
	);
	end = clock_us(CLOCK_MONOTONIC) + (uint64_t)seconds * SECOND;
	for (;;) {
		uint64_t totals[4] = { 0 }, ticks, mono;
		unsigned long rss = 0U;
		unsigned long long cpu[8];
		FILE *file;
		int cpu_index;

		if (program_fd >= 0) {
			/* Output lengths from an earlier query are input buffer capacities. */
			program = (struct bpf_prog_info){ 0 };
			size = sizeof(program);
			require(bpf_prog_get_info_by_fd(program_fd, &program, &size) == 0,
				"program stats");
			require(bpf_map_lookup_elem(counters_fd, &key, values) == 0,
				"counter read");
			for (cpu_index = 0; cpu_index < cpus; cpu_index++)
				for (index = 0U; index < words; index++)
					totals[index] += values[(size_t)cpu_index * words + index];
		}
		ticks = daemon_ticks(pid, &rss);
		file = fopen(PROC_STAT_PATH, FILE_MODE);
		require(file != NULL && fscanf(file,
					       "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
					       &cpu[0],
					       &cpu[1],
					       &cpu[2],
					       &cpu[3],
					       &cpu[4],
					       &cpu[5],
					       &cpu[6],
					       &cpu[7]) == 8,
			"host CPU");
		(void)fclose(file);
		mono = clock_us(CLOCK_MONOTONIC);
		printf("%" PRIu64 " %" PRIu64 " %" PRIu64 " %lu %llu %llu %" PRIu64 " %" PRIu64
		       " %" PRIu64 " %" PRIu64,
		       clock_us(CLOCK_REALTIME),
		       mono,
		       ticks,
		       rss,
		       (unsigned long long)program.run_cnt,
		       (unsigned long long)program.run_time_ns,
		       totals[0],
		       totals[1],
		       totals[2],
		       totals[3]);
		for (index = 0U; index < ARRAY_SIZE(cpu); index++)
			printf(" %llu", cpu[index]);
		putchar('\n');
		(void)fflush(stdout);
		if (stopping || mono >= end)
			break;
		usleep(500U * MILLISECOND);
	}
	free(values);
	if (counters_fd >= 0)
		(void)close(counters_fd);
	if (program_fd >= 0)
		(void)close(program_fd);
	(void)close(stats_fd);
	return 0;
}
