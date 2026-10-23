#ifndef PLATFORM_INSTANCE_H_INCLUDED
#define PLATFORM_INSTANCE_H_INCLUDED

#include <stddef.h>

/*
 * One daemon per instance name: a second one would control the same CAKE
 * qdiscs. The lock lasts until the process exits, so its descriptor is never
 * closed; it is close-on-exec, so pinger children do not hold it. A failure
 * returns -1 with the reason, and the running instance's pid when known, in
 * error.
 */
int instance_lock(const char *section_name, char *error, size_t error_size);

#endif
