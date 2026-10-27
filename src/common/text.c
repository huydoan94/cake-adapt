#include "common/text.h"
#include "constants/text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool parse_unsigned(const char *start, const char *end, uint64_t *value)
{
	char *tail;
	unsigned long long parsed;

	if (start == end || strspn(start, DECIMAL_DIGITS) != (size_t)(end - start))
		return false;
	errno = 0;
	parsed = strtoull(start, &tail, 10);
	if (errno == ERANGE || tail != end)
		return false;
	*value = (uint64_t)parsed;
	return true;
}
