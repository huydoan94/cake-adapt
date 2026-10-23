#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/pinger.h"
#include "common/error.h"
#include "constants/latency.h"
#include "constants/options.h"
#include "constants/units.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <stdbool.h>
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
		.tv_nsec = (long)(CHILD_STOP_INTERVAL_US * NANOSECONDS_PER_MICROSECOND),
	};
	unsigned int attempt;

	if (process_identifier <= 0)
		return;

	/* A prefix may launch fping as a child; terminate the owned group too. */
	(void)kill(-process_identifier, SIGTERM);
	for (attempt = 0U; attempt < CHILD_STOP_ATTEMPTS; attempt++) {
		pid_t result = waitpid(process_identifier, NULL, WNOHANG);

		/* Reaped, or an error other than an interruption, such as ECHILD. */
		if (result == process_identifier || (result < 0 && errno != EINTR))
			return;
		(void)nanosleep(&interval, NULL);
	}

	(void)kill(-process_identifier, SIGKILL);
	while (waitpid(process_identifier, NULL, 0) < 0 && errno == EINTR) {
	}
}

/* Returns 0 or the error number, like posix_spawn(). */
static int spawn_child(char *const arguments[], const int output_pipe[2], pid_t *process_identifier)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t child_signal_mask;
	sigset_t default_signals;
	int result;

	result = posix_spawn_file_actions_init(&actions);
	if (result != 0)
		return result;
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
	/*
	 * uloop ignores SIGPIPE, and an ignored signal survives exec. With the
	 * default restored, a pinger whose reader died (even by SIGKILL) exits on
	 * its next write instead of running on as an orphan.
	 */
	(void)sigemptyset(&default_signals);
	(void)sigaddset(&default_signals, SIGPIPE);

	result = posix_spawnattr_setsigmask(&attributes, &child_signal_mask);
	if (result == 0)
		result = posix_spawnattr_setsigdefault(&attributes, &default_signals);
	if (result == 0)
		result = posix_spawnattr_setpgroup(&attributes, 0);
	if (result == 0) {
		result = posix_spawnattr_setflags(
			&attributes,
			POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP
		);
	}
	if (result == 0) {
		result = posix_spawnp(
			process_identifier,
			arguments[0],
			&actions,
			&attributes,
			arguments,
			environ
		);
	}

	(void)posix_spawnattr_destroy(&attributes);
destroy_actions:
	(void)posix_spawn_file_actions_destroy(&actions);
	return result;
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
	const struct latency *latency,
	size_t fixed,
	char *error,
	size_t error_size
)
{
	const struct latency_settings *settings = &latency->settings;
	const char *prefix = settings->prefix;
	const wordexp_t *prefix_words = &command->prefix;
	const wordexp_t *extra_words = &command->extra;
	size_t count;
	size_t index;

	*command = (struct pinger_command){ 0 };
	if (expand_words(
		    settings->extra_arguments,
		    OPTION_PING_EXTRA_ARGS,
		    &command->extra,
		    error,
		    error_size
	    ) != 0)
		goto fail;
	if (expand_words(prefix, OPTION_PING_PREFIX_STRING, &command->prefix, error, error_size) !=
	    0)
		goto fail;
	/* A prefix names the program to run, so a set one must leave at least a word. */
	if (prefix[0] != '\0' && prefix_words->we_wordc == 0U) {
		error_set(error, error_size, "could not parse %s", OPTION_PING_PREFIX_STRING);
		goto fail;
	}
	/* One more for the terminating NULL. */
	count = prefix_words->we_wordc + extra_words->we_wordc + fixed + 1U;
	command->argv = calloc(count, sizeof(*command->argv));
	if (command->argv == NULL) {
		error_set(
			error,
			error_size,
			"could not allocate %s arguments: %s",
			latency->ops->name,
			strerror(errno)
		);
		goto fail;
	}
	for (index = 0U; index < prefix_words->we_wordc; index++)
		pinger_command_add(command, prefix_words->we_wordv[index]);
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

int start_child(
	struct latency *latency,
	size_t child_index,
	char *const arguments[],
	char *error,
	size_t error_size
)
{
	struct latency_child *child = &latency->children[child_index];
	const char *name = latency->ops->name;
	int output_pipe[2];
	pid_t process_identifier;
	int result;

	if (pipe2(output_pipe, O_CLOEXEC) != 0) {
		return error_set(
			error,
			error_size,
			"could not create %s pipe: %s",
			name,
			strerror(errno)
		);
	}
	result = spawn_child(arguments, output_pipe, &process_identifier);
	if (result != 0) {
		(void)close(output_pipe[0]);
		(void)close(output_pipe[1]);
		return error_set(error, error_size, "could not start %s: %s", name, strerror(result));
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

size_t latency_child_count(const struct latency *latency)
{
	return latency->child_count;
}

int latency_child_descriptor(const struct latency *latency, size_t child_index)
{
	const struct latency_child *child;

	if (child_index >= latency->child_count)
		return -1;
	child = &latency->children[child_index];
	return child->output_descriptor;
}

pid_t latency_child_process(const struct latency *latency, size_t child_index)
{
	const struct latency_child *child = &latency->children[child_index];

	return child->process_identifier;
}

bool latency_stopping(const struct latency *latency)
{
	for (size_t index = 0U; index < CONFIG_MAX_REFLECTORS; index++) {
		const struct latency_child *child = &latency->children[index];

		if (child->stopping)
			return true;
	}
	return false;
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
