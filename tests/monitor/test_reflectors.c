/* The reflectors part's recent added delay, used as the TCP queue bound. */
#include "monitor/reflectors.c"

#include <assert.h>
#include <stdio.h>

#define MILLISECOND MILLISECOND

static struct config config = { .no_pingers = 6U };
static struct monitor monitor = { .config = &config };

/* Every slot replies once at time, with the given added round-trip delays (ms). */
static int64_t replies(const int64_t delays_ms[6], uint64_t time)
{
	struct monitor_reflectors *reflectors = &monitor.reflectors;

	for (size_t slot = 0U; slot < 6U; slot++)
		recent_delay_add(
			&reflectors->recent[slot],
			delays_ms[slot] * (int64_t)MILLISECOND,
			time + slot * 50U * MILLISECOND
		);
	return reflectors_recent_delay_us(&monitor, time + SECOND / 2U);
}

/* One slot keeps its largest delay over the current and previous second. */
static void one_slot_window(void)
{
	struct reflector_recent_delay recent = { 0 };
	int64_t value;

	recent_delay_add(&recent, 30 * (int64_t)MILLISECOND, 10U * SECOND);
	/* A low reply in the same second leaves it. */
	recent_delay_add(&recent, 5 * (int64_t)MILLISECOND, 10U * SECOND + 500U * MILLISECOND);
	assert(recent_delay_value(&recent, 10U, &value) && value == 30 * (int64_t)MILLISECOND);
	/* The previous second still counts. */
	recent_delay_add(&recent, 4 * (int64_t)MILLISECOND, 11U * SECOND);
	assert(recent_delay_value(&recent, 11U, &value) && value == 30 * (int64_t)MILLISECOND);
	/* Once the high second is two seconds old, it falls. */
	recent_delay_add(&recent, 3 * (int64_t)MILLISECOND, 12U * SECOND);
	assert(recent_delay_value(&recent, 12U, &value) && value == 4 * (int64_t)MILLISECOND);
	/* A slot that did not reply in the current or previous second does not count. */
	assert(!recent_delay_value(&recent, 14U, &value));
	/* After a gap without replies, only the new second counts. */
	recent_delay_add(&recent, 7 * (int64_t)MILLISECOND, 20U * SECOND);
	assert(recent_delay_value(&recent, 20U, &value) && value == 7 * (int64_t)MILLISECOND);
}

/*
 * The router pattern of 2026-10-06: one reflector answering 50 ms late among
 * prompt ones opened the bound, and the TCP upload estimate filled it.
 */
static void median_across_slots(void)
{
	const int64_t prompt[6] = { 3, 2, 4, 3, 2, 3 };
	const int64_t one_slow[6] = { 3, 2, 50, 3, 2, 3 };
	const int64_t queue[6] = { 31, 30, 33, 30, 32, 30 };
	const int64_t high_baseline[6] = { 0, 30, 31, 30, 32, 30 };

	assert(reflectors_recent_delay_us(&monitor, 5U * SECOND) == -1);
	assert(replies(prompt, 10U * SECOND) == 3 * (int64_t)MILLISECOND);
	assert(replies(one_slow, 11U * SECOND) == 3 * (int64_t)MILLISECOND);
	/* A queue on the access link delays every reflector, so the bound rises. */
	assert(replies(queue, 20U * SECOND) == 30 * (int64_t)MILLISECOND);
	/* One reflector whose baseline sits too high does not hold the bound down. */
	assert(replies(high_baseline, 30U * SECOND) == 30 * (int64_t)MILLISECOND);
	/* Two seconds after the last replies, no slot counts. */
	assert(reflectors_recent_delay_us(&monitor, 32U * SECOND) == -1);
}

int main(void)
{
	one_slot_window();
	median_across_slots();
	puts("monitor reflector delay tests passed");
	return 0;
}
