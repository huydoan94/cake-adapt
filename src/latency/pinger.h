#ifndef LATENCY_PINGER_H_INCLUDED
#define LATENCY_PINGER_H_INCLUDED

/* Private interface between latency session ownership and pinger backends. */

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <wordexp.h>

#include "latency/latency.h"

/* Terminates the child's process group, escalating to SIGKILL, and reaps it. */
void stop_child(pid_t process_identifier);

/* Frees and clears words on failure; an empty value expands to no words. */
int expand_words(const char *value, bool require_word, const char *option, wordexp_t *words,
		 char *error, size_t error_size);

int validate_targets(const char *const *targets, size_t target_count, char *error,
		     size_t error_size);

/*
 * Starts arguments[0] in its own process group with stdout on a nonblocking
 * pipe owned by child and stderr on /dev/null.
 */
int start_child(struct latency_child *child, char *const arguments[], const char *name, char *error,
		size_t error_size);

/* Schedule a reaped IRTT session's restart, delayed after a fast exit. */
enum latency_probe_result schedule_irtt_restart(struct latency_child *child, char *error,
						size_t error_size);

#endif
