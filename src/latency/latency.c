#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/pinger.h"
#include "common/error.h"
#include "common/constants.h"
#include "config/defaults.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <spawn.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

extern char **environ;

static int set_nonblocking(int descriptor, const char *name, char *error, size_t error_size)
{
	int flags = fcntl(descriptor, F_GETFL);

	if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
		return error_set(
			error,
			error_size,
			"could not make %s pipe nonblocking: %s",
			name,
			strerror(errno)
		);
	}
	return 0;
}

/* Terminates the child's process group, escalating to SIGKILL, and reaps it. */
static void stop_child(pid_t process_identifier)
{
	const struct timespec interval = {
		.tv_sec = 0,
		.tv_nsec = CHILD_STOP_INTERVAL_NANOSECONDS,
	};
	unsigned int attempt;

	if (process_identifier <= 0)
		return;

	/* A prefix may launch fping as a child; terminate the owned group too. */
	(void)kill(-process_identifier, SIGTERM);
	for (attempt = 0U; attempt < CHILD_STOP_ATTEMPTS; attempt++) {
		pid_t result = waitpid(process_identifier, NULL, WNOHANG);

		if (result == process_identifier || (result < 0 && errno == ECHILD))
			return;
		if (result < 0 && errno != EINTR)
			return;
		(void)nanosleep(&interval, NULL);
	}

	(void)kill(-process_identifier, SIGKILL);
	while (waitpid(process_identifier, NULL, 0) < 0 && errno == EINTR) {
	}
}

static int spawn_child(
	pid_t *process_identifier,
	const int output_pipe[2],
	const char *executable,
	char *const arguments[],
	const char *name,
	char *error,
	size_t error_size
)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t child_signal_mask;
	int result;

	result = posix_spawn_file_actions_init(&actions);
	if (result != 0)
		goto failed;
	if ((result = posix_spawn_file_actions_addclose(&actions, output_pipe[0])) != 0 ||
	    (result = posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO)) !=
		    0 ||
	    (result = posix_spawn_file_actions_addclose(&actions, output_pipe[1])) != 0 ||
	    (result = posix_spawn_file_actions_addopen(
		     &actions,
		     STDERR_FILENO,
		     NULL_DEVICE_PATH,
		     O_WRONLY,
		     0
	     )) != 0) {
		goto destroy_actions;
	}

	result = posix_spawnattr_init(&attributes);
	if (result != 0)
		goto destroy_actions;
	(void)sigemptyset(&child_signal_mask);

	result = posix_spawnattr_setsigmask(&attributes, &child_signal_mask);
	if (result == 0)
		result = posix_spawnattr_setpgroup(&attributes, 0);
	if (result == 0) {
		result = posix_spawnattr_setflags(
			&attributes,
			POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP
		);
	}
	if (result == 0) {
		result = posix_spawnp(
			process_identifier,
			executable,
			&actions,
			&attributes,
			arguments,
			environ
		);
	}

	(void)posix_spawnattr_destroy(&attributes);
destroy_actions:
	(void)posix_spawn_file_actions_destroy(&actions);
failed:
	if (result != 0) {
		return error_set(error, error_size, "could not start %s: %s", name, strerror(result));
	}
	return 0;
}

/* Frees and clears words on failure; an empty value expands to no words. */
static int
expand_words(const char *value, const char *option, wordexp_t *words, char *error, size_t error_size)
{
	int result;

	if (value[0] == '\0')
		return 0;
	result = wordexp(value, words, WRDE_NOCMD);
	if (result == 0)
		return 0;
	if (result == WRDE_NOSPACE) {
		wordfree(words);
		*words = (wordexp_t){ 0 };
	}
	return error_set(error, error_size, "could not parse %s", option);
}

int pinger_command_init(
	struct pinger_command *command,
	const char *prefix,
	const char *extra,
	size_t fixed,
	const char *name,
	char *error,
	size_t error_size
)
{
	size_t count;
	size_t index;

	*command = (struct pinger_command){ 0 };
	if (expand_words(extra, OPTION_PING_EXTRA_ARGS, &command->extra, error, error_size) != 0 ||
	    expand_words(prefix, OPTION_PING_PREFIX_STRING, &command->prefix, error, error_size) !=
		    0)
		goto fail;
	/* A prefix names the program to run, so a set one must leave at least a word. */
	if (prefix[0] != '\0' && command->prefix.we_wordc == 0U) {
		error_set(error, error_size, "could not parse %s", OPTION_PING_PREFIX_STRING);
		goto fail;
	}
	/* One more for the terminating NULL. */
	count = command->prefix.we_wordc + command->extra.we_wordc + fixed + 1U;
	command->argv = calloc(count, sizeof(*command->argv));
	if (command->argv == NULL) {
		error_set(
			error,
			error_size,
			"could not allocate %s arguments: %s",
			name,
			strerror(errno)
		);
		goto fail;
	}
	for (index = 0U; index < command->prefix.we_wordc; index++)
		pinger_command_add(command, command->prefix.we_wordv[index]);
	return 0;

fail:
	pinger_command_free(command);
	return -1;
}

void pinger_command_free(struct pinger_command *command)
{
	free(command->argv);
	wordfree(&command->prefix);
	wordfree(&command->extra);
	*command = (struct pinger_command){ 0 };
}

int validate_targets(const char *const *targets, size_t target_count, char *error, size_t error_size)
{
	size_t index;

	for (index = 0U; index < target_count; index++) {
		if (!target_is_valid(targets[index])) {
			return error_set(
				error,
				error_size,
				"latency target '%s' is not a valid IP address or hostname",
				targets[index]
			);
		}
	}
	return 0;
}

int start_child(
	struct latency_child *child,
	char *const arguments[],
	const char *name,
	char *error,
	size_t error_size
)
{
	int output_pipe[2];
	pid_t process_identifier;

	if (pipe2(output_pipe, O_CLOEXEC) != 0) {
		return error_set(
			error,
			error_size,
			"could not create %s pipe: %s",
			name,
			strerror(errno)
		);
	}
	if (spawn_child(
		    &process_identifier,
		    output_pipe,
		    arguments[0],
		    arguments,
		    name,
		    error,
		    error_size
	    ) != 0) {
		(void)close(output_pipe[0]);
		(void)close(output_pipe[1]);
		return -1;
	}
	(void)close(output_pipe[1]);
	if (set_nonblocking(output_pipe[0], name, error, error_size) != 0) {
		(void)close(output_pipe[0]);
		stop_child(process_identifier);
		return -1;
	}
	child->output_descriptor = output_pipe[0];
	child->process_identifier = process_identifier;
	return 0;
}

void latency_init(struct latency *latency)
{
	size_t index;

	*latency = (struct latency){ .ops = &fping_ops };
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		latency->children[index].output_descriptor = -1;
		latency->children[index].process_identifier = -1;
	}
}

/* Every pinger backend and the executable it launches. */
static const struct {
	const char *method;
	const char *executable;
} backends[] = {
	{ PINGER_METHOD_FPING, FPING_PATH },
	{ PINGER_METHOD_FPING_TS, FPING_PATH },
	{ PINGER_METHOD_IRTT, IRTT_PATH },
};

const char *latency_backend_executable(const char *pinger_method)
{
	size_t index;

	for (index = 0U; index < ARRAY_SIZE(backends); index++)
		if (strcmp(backends[index].method, pinger_method) == 0)
			return backends[index].executable;
	return NULL;
}

int latency_check_backend(const char *pinger_method, char *error, size_t error_size)
{
	const char *executable = latency_backend_executable(pinger_method);

	if (executable == NULL)
		return error_set(error, error_size, "unknown pinger_method '%s'", pinger_method);
	if (access(executable, X_OK) != 0) {
		return error_set(
			error,
			error_size,
			"ping binary %s for pinger_method '%s' is not available: %s",
			executable,
			pinger_method,
			strerror(errno)
		);
	}
	return 0;
}

bool target_is_valid(const char *target)
{
	size_t length = strlen(target);

	/* An address or hostname; it never starts with '-', so fping cannot read it as an option. */
	return length > 0U && length < LATENCY_TARGET_SIZE &&
	       (target[0] == ':' || isalnum((unsigned char)target[0])) &&
	       strspn(target, TARGET_CHARACTERS) == length;
}

bool latency_is_open(const struct latency *latency)
{
	return latency->active;
}

size_t latency_child_count(const struct latency *latency)
{
	return latency->child_count;
}

int latency_child_descriptor(const struct latency *latency, size_t child_index)
{
	if (child_index >= latency->child_count)
		return -1;
	return latency->children[child_index].output_descriptor;
}

pid_t latency_child_process(const struct latency *latency, size_t child_index)
{
	return latency->children[child_index].process_identifier;
}

bool latency_stopping(const struct latency *latency)
{
	size_t index;

	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++)
		if (latency->children[index].stopping)
			return true;
	return false;
}

void latency_close(struct latency *latency)
{
	size_t index;

	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct latency_child *child = &latency->children[index];

		if (child->output_descriptor >= 0)
			(void)close(child->output_descriptor);
		child->output_descriptor = -1;
		child->target = NULL;
		/* A prefix may launch the pinger as a child; terminate the owned group too. */
		if (child->process_identifier > 0 && !child->stopping) {
			(void)kill(-child->process_identifier, SIGTERM);
			child->stopping = true;
		}
	}
	latency->active = false;
	latency->ping_extra_args = NULL;
	latency->ping_prefix_string = NULL;
	latency->child_count = 0U;
}

void latency_kill_stopping(struct latency *latency)
{
	size_t index;

	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		const struct latency_child *child = &latency->children[index];

		if (child->stopping)
			(void)kill(-child->process_identifier, SIGKILL);
	}
}

void latency_stop_now(struct latency *latency)
{
	size_t index;

	latency_close(latency);
	for (index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		struct latency_child *child = &latency->children[index];

		stop_child(child->process_identifier);
		child->process_identifier = -1;
		child->stopping = false;
	}
}

enum latency_line_result
latency_next_line(const char *data, size_t length, char line[LATENCY_OUTPUT_SIZE], size_t *consumed)
{
	const char *newline =
		memchr(data, '\n', length < LATENCY_OUTPUT_SIZE ? length : LATENCY_OUTPUT_SIZE);
	size_t line_length;

	if (newline == NULL) {
		return length < LATENCY_OUTPUT_SIZE ? LATENCY_LINE_INCOMPLETE :
						      LATENCY_LINE_TOO_LONG;
	}
	line_length = (size_t)(newline - data);
	*consumed = line_length + 1U;
	if (line_length > 0U && data[line_length - 1U] == '\r')
		line_length--;
	/* The newline occupies one of the LATENCY_OUTPUT_SIZE bytes, leaving room for NUL. */
	memcpy(line, data, line_length);
	line[line_length] = '\0';
	return LATENCY_LINE_READY;
}

enum latency_probe_result latency_handle_line(
	const struct latency *latency,
	size_t child_index,
	const char *line,
	struct latency_sample *sample,
	char *error,
	size_t error_size
)
{
	return latency->ops->parse(&latency->children[child_index], line, sample, error, error_size);
}

enum latency_probe_result latency_child_exited(
	struct latency *latency,
	size_t child_index,
	int status,
	char *error,
	size_t error_size
)
{
	struct latency_child *child = &latency->children[child_index];
	const char *name = latency->ops->name;
	bool stopping = child->stopping;

	child->process_identifier = -1;
	child->stopping = false;
	if (stopping)
		return LATENCY_PROBE_STOPPED;
	if (child->output_descriptor >= 0)
		(void)close(child->output_descriptor);
	child->output_descriptor = -1;
	if (WIFEXITED(status))
		error_set(error, error_size, "%s exited with status %d", name, WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		error_set(error, error_size, "%s terminated by signal %d", name, WTERMSIG(status));
	else
		error_set(error, error_size, "%s stopped unexpectedly", name);
	return latency->ops->exited(child, error, error_size);
}
