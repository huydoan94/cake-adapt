#include "common/random.h"

#include <errno.h>

bool random_below(size_t count, random_u32_source source, void *context, size_t *index)
{
	uint32_t value;
	uint32_t limit;

	if (count == 0U || count > (size_t)UINT32_MAX || source == NULL || index == NULL) {
		errno = EINVAL;
		return false;
	}
	if (count == 1U) {
		*index = 0U;
		return true;
	}

	limit = UINT32_MAX - (UINT32_MAX % (uint32_t)count) - 1U;
	do {
		if (!source(&value, context))
			return false;
	} while (value > limit);

	*index = (size_t)(value % (uint32_t)count);
	return true;
}

bool shuffle(size_t *items, size_t count, random_u32_source source, void *context)
{
	size_t position;

	if (items == NULL || source == NULL) {
		errno = EINVAL;
		return false;
	}
	for (position = count; position > 1U; position--) {
		size_t replacement;
		size_t current = position - 1U;
		size_t temporary;

		if (!random_below(position, source, context, &replacement))
			return false;
		temporary = items[current];
		items[current] = items[replacement];
		items[replacement] = temporary;
	}
	return true;
}
