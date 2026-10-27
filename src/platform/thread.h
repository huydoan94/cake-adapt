#ifndef PLATFORM_THREAD_H_INCLUDED
#define PLATFORM_THREAD_H_INCLUDED

/* Bounded joins of the daemon's helper threads. */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Joins THREAD if it exits within bound_us of CLOCK_MONOTONIC, checking every 10 ms, so a
 * step of the wall clock cannot stretch the wait. The number of checks also bounds it when
 * the clock cannot be read. False leaves the thread running and joinable.
 */
bool thread_join_within(pthread_t thread, uint64_t bound_us);

#endif
