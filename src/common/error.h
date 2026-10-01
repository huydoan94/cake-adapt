#ifndef ERROR_H_INCLUDED
#define ERROR_H_INCLUDED

#include <stddef.h>

#define ERROR_SIZE 256U

void error_set(
    char *error,
    size_t error_size,
    const char *format,
    ...
) __attribute__((format(printf, 3, 4)));

#endif
