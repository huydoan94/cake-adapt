#ifndef CAKE_ACCOUNTING_H_INCLUDED
#define CAKE_ACCOUNTING_H_INCLUDED

#include <linux/pkt_sched.h>
#include <linux/types.h>

/* Shared with the socket filter; fixed-width fields keep its map ABI stable. */
struct cake_accounting {
	__s32 overhead_bytes;
	__u32 mpu_bytes;
	__u32 atm_mode;
	__u32 raw;
};

_Static_assert(sizeof(struct cake_accounting) == 16, "CAKE accounting layout");

/*
 * Linux cake_calc_overhead(), for one non-GSO packet. The caller has validated
 * network_offset <= length and the framing mode. RAW keeps the link header;
 * overhead, MPU and framing still apply. No scheduling is reproduced here.
 */
/* i386 BPF JIT requires inline calls and 32-bit division, like CAKE itself. */
static inline __attribute__((always_inline)) __u32
cake_accounted_bytes(const struct cake_accounting *accounting, __u32 length, __u32 network_offset)
{
	__s64 adjusted = (__s64)(accounting->raw ? length : length - network_offset) +
			 accounting->overhead_bytes;
	__u32 bytes;

	if (adjusted < (__s64)accounting->mpu_bytes)
		adjusted = (__s64)accounting->mpu_bytes;
	bytes = (__u32)adjusted;
	if (accounting->atm_mode == CAKE_ATM_ATM)
		return (bytes + 47U) / 48U * 53U;
	if (accounting->atm_mode == CAKE_ATM_PTM)
		return bytes + (bytes + 63U) / 64U;
	return bytes;
}

#endif
