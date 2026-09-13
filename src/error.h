#ifndef SQM_MON_ERROR_H
#define SQM_MON_ERROR_H

#include <stddef.h>

void error_set(
    char *error,
    size_t error_size,
    const char *format,
    ...
) __attribute__((format(printf, 3, 4)));

#endif
