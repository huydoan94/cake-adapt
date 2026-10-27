#ifndef PLATFORM_PATH_H_INCLUDED
#define PLATFORM_PATH_H_INCLUDED

/* Paths: absolute names and the pid lock file. */

#include <stddef.h>

/*
 * PATH as an absolute path in BUFFER: copied when it is absolute, otherwise
 * joined to the current directory (the path need not exist). Returns -1 with
 * errno set when the current directory is unknown or the result does not fit.
 */
int path_absolute(const char *path, char *buffer, size_t size);

/*
 * Takes an exclusive lock on PATH, creating it, and writes this process's pid
 * into it. Returns the descriptor, which holds the lock until it is closed or
 * the process exits, or -1 with errno set. EWOULDBLOCK means another process
 * holds the lock; holder_pid is then its pid, or 0 when that is unreadable.
 */
int path_lock_pid(const char *path, long *holder_pid);

#endif
