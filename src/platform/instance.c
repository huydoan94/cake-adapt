#define _POSIX_C_SOURCE 200809L

#include "platform/instance.h"
#include "common/error.h"
#include "common/helpers.h"
#include "constants/program.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int instance_lock(const char *section_name, char *error, size_t error_size)
{
	char path[PATH_MAX];
	long holder_pid;
	int length = snprintf(
		path,
		sizeof(path),
		"%s/%s" LOCK_EXTENSION,
		RUNTIME_DIRECTORY,
		section_name
	);

	if (length < 0 || (size_t)length >= sizeof(path))
		return error_set(
			error,
			error_size,
			"instance '%s': lock path is too long",
			section_name
		);
	if (mkdir(RUNTIME_DIRECTORY, 0700) != 0 && errno != EEXIST) {
		return error_set(
			error,
			error_size,
			"could not create %s: %s; exiting",
			RUNTIME_DIRECTORY,
			strerror(errno)
		);
	}
	if (lock_pid_file(path, &holder_pid) >= 0)
		return 0;
	if (errno != EWOULDBLOCK)
		return error_set(
			error,
			error_size,
			"could not lock %s: %s; exiting",
			path,
			strerror(errno)
		);
	if (holder_pid > 0)
		return error_set(
			error,
			error_size,
			"instance '%s' is already running (pid %ld); exiting",
			section_name,
			holder_pid
		);
	return error_set(
		error,
		error_size,
		"instance '%s' is already running; exiting",
		section_name
	);
}
