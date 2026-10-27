/* pthread_tryjoin_np */
#define _GNU_SOURCE

#include "platform/thread.h"
#include "common/clock.h"
#include "common/convert.h"
#include "constants/program.h"

#include <time.h>

bool thread_join_within(pthread_t thread, uint64_t bound_us)
{
	const struct timespec step = us_to_timespec(THREAD_JOIN_CHECK_US);
	uint64_t start_us = 0U;
	uint64_t now_us;
	uint64_t check;
	/* Without a start time, only the number of checks bounds the wait. */
	bool timed = read_clock_us(CLOCK_MONOTONIC, &start_us);

	for (check = 0U; check <= bound_us / THREAD_JOIN_CHECK_US; check++) {
		if (pthread_tryjoin_np(thread, NULL) == 0)
			return true;
		if (timed && read_clock_us(CLOCK_MONOTONIC, &now_us) &&
		    now_us - start_us >= bound_us)
			break;
		(void)clock_nanosleep(CLOCK_MONOTONIC, 0, &step, NULL);
	}
	return false;
}
