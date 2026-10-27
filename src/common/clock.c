#define _GNU_SOURCE

#include "common/clock.h"
#include "common/convert.h"

bool read_clock_us(clockid_t clock_identifier, uint64_t *timestamp_us)
{
	struct timespec value;

	if (clock_gettime(clock_identifier, &value) != 0 || value.tv_sec < 0)
		return false;
	*timestamp_us = timespec_to_us(&value);
	return true;
}
