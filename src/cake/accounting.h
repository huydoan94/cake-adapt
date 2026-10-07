#ifndef CAKE_ACCOUNTING_H_INCLUDED
#define CAKE_ACCOUNTING_H_INCLUDED

#include <linux/pkt_sched.h>
#include <linux/types.h>

/* ATM sends 48 payload bytes in each 53-byte cell; PTM adds a byte per 64. */
#define CAKE_ATM_CELL_PAYLOAD_BYTES 48U
#define CAKE_ATM_CELL_BYTES 53U
#define CAKE_PTM_BLOCK_BYTES 64U

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
	__s64 adjusted_bytes = (__s64)(accounting->raw ? length : length - network_offset) +
			       accounting->overhead_bytes;
	__u32 bytes;

	if (adjusted_bytes < (__s64)accounting->mpu_bytes)
		adjusted_bytes = (__s64)accounting->mpu_bytes;
	bytes = (__u32)adjusted_bytes;
	if (accounting->atm_mode == CAKE_ATM_ATM)
		return (bytes + CAKE_ATM_CELL_PAYLOAD_BYTES - 1U) / CAKE_ATM_CELL_PAYLOAD_BYTES *
		       CAKE_ATM_CELL_BYTES;
	if (accounting->atm_mode == CAKE_ATM_PTM)
		return bytes + (bytes + CAKE_PTM_BLOCK_BYTES - 1U) / CAKE_PTM_BLOCK_BYTES;
	return bytes;
}

#endif
