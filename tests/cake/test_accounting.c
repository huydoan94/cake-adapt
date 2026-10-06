#include "cake/accounting.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
	struct cake_accounting model = { .overhead_bytes = 44U, .atm_mode = CAKE_ATM_NONE };

	/* A 66-byte Ethernet timestamp ACK has 52 network bytes. */
	assert(cake_accounted_bytes(&model, 66U, 14U) == 96U);
	assert(cake_accounted_bytes(&model, 1514U, 14U) == 1544U);
	assert(cake_accounted_bytes(&model, 70U, 18U) == 96U);
	assert(cake_accounted_bytes(&model, 74U, 22U) == 96U);
	model.raw = 1U;
	assert(cake_accounted_bytes(&model, 66U, 14U) == 110U);
	model.raw = 0U;
	model.overhead_bytes = -12;
	assert(cake_accounted_bytes(&model, 66U, 14U) == 40U);
	model.mpu_bytes = 84U;
	assert(cake_accounted_bytes(&model, 66U, 14U) == 84U);
	model.overhead_bytes = -64;
	model.mpu_bytes = 0U;
	assert(cake_accounted_bytes(&model, 66U, 14U) == 0U);
	model.overhead_bytes = 0;
	model.atm_mode = CAKE_ATM_ATM;
	assert(cake_accounted_bytes(&model, 62U, 14U) == 53U);
	assert(cake_accounted_bytes(&model, 63U, 14U) == 106U);
	assert(cake_accounted_bytes(&model, 110U, 14U) == 106U);
	assert(cake_accounted_bytes(&model, 111U, 14U) == 159U);
	model.mpu_bytes = 84U;
	assert(cake_accounted_bytes(&model, 54U, 14U) == 106U);
	model.mpu_bytes = 0U;
	model.atm_mode = CAKE_ATM_PTM;
	assert(cake_accounted_bytes(&model, 78U, 14U) == 65U);
	assert(cake_accounted_bytes(&model, 79U, 14U) == 67U);
	model.raw = 1U;
	model.overhead_bytes = 10;
	assert(cake_accounted_bytes(&model, 54U, 14U) == 65U);
	puts("CAKE packet accounting tests passed");
	return 0;
}
