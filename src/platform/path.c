#define _GNU_SOURCE

#include "platform/path.h"
#include "common/text.h"
#include "constants/program.h"
#include "constants/text.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

int path_absolute(const char *path, char *buffer, size_t size)
{
	char directory[PATH_MAX];
	int length;

	if (path[0] == '/') {
		length = snprintf(buffer, size, "%s", path);
	} else {
		if (getcwd(directory, sizeof(directory)) == NULL)
			return -1;
		/* The root directory already ends in the separator. */
		length = snprintf(
			buffer,
			size,
			"%s/%s",
			strcmp(directory, "/") == 0 ? EMPTY_STRING : directory,
			path
		);
	}
	if (length < 0 || (size_t)length >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

/* The pid of the process holding the lock on descriptor, or 0. */
static long lock_holder_pid(int descriptor)
{
	char text[LOCK_PID_TEXT_SIZE];
	ssize_t size = pread(descriptor, text, sizeof(text) - 1U, 0);
	uint64_t pid;

	if (size <= 0)
		return 0;
	text[size] = '\0';
	if (!parse_unsigned(text, text + strspn(text, DECIMAL_DIGITS), &pid) || pid > LONG_MAX)
		return 0;
	return (long)pid;
}

int path_lock_pid(const char *path, long *holder_pid)
{
	char text[LOCK_PID_TEXT_SIZE];
	int descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	int length;
	int error;

	*holder_pid = 0;
	if (descriptor < 0)
		return -1;
	if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
		error = errno;
		if (error == EWOULDBLOCK)
			*holder_pid = lock_holder_pid(descriptor);
		goto fail;
	}
	length = snprintf(text, sizeof(text), "%ld\n", (long)getpid());
	if (ftruncate(descriptor, 0) == 0 && pwrite(descriptor, text, (size_t)length, 0) == length)
		return descriptor;
	error = errno;
fail:
	(void)close(descriptor);
	errno = error;
	return -1;
}
