/*
 * Replaces the target architecture's asm/byteorder.h for the BPF filter only.
 * Some architectures choose their byte order from compiler macros that the BPF
 * target does not define (MIPS needs __MIPSEB__ or __MIPSEL__ and stops with
 * an error without them). -target bpfeb or bpfel already matches the router,
 * so take the byte order from the compiler instead.
 */
#ifndef TCPDELAY_ASM_BYTEORDER_H
#define TCPDELAY_ASM_BYTEORDER_H

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#include <linux/byteorder/big_endian.h>
#else
#include <linux/byteorder/little_endian.h>
#endif

#endif
