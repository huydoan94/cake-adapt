#ifndef COMMON_TEXT_H_INCLUDED
#define COMMON_TEXT_H_INCLUDED

/* Decimal parsing shared by the configuration and pinger output parsers. */

#include <stdbool.h>
#include <stdint.h>

/* Decimal token in a NUL-terminated string; end points to its delimiter. */
bool parse_unsigned(const char *start, const char *end, uint64_t *value);

#endif
