#ifndef COMMON_RANDOM_H_INCLUDED
#define COMMON_RANDOM_H_INCLUDED

/* Uniform selection and shuffling from a caller-supplied entropy source. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*random_u32_source)(uint32_t *value, void *context);

/* Uniformly select an index below count using the supplied entropy source. */
bool random_below(size_t count, random_u32_source source, void *context, size_t *index);

/* Shuffle in place; callers needing failure atomicity provide a working copy. */
bool shuffle(size_t *items, size_t count, random_u32_source source, void *context);

#endif
