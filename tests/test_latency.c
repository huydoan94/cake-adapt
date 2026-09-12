#include "latency.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_initial_state_is_closed(void)
{
    struct sqm_mon_latency latency;

    latency_init(&latency);

    assert(latency.socket_descriptor == -1);
}

static void test_invalid_target_is_rejected_before_opening_socket(void)
{
    struct sqm_mon_latency latency;
    char error[256] = "";

    latency_init(&latency);

    assert(latency_open(
        &latency,
        "lo",
        "not-an-ip-address",
        error,
        sizeof(error)
    ) != 0);
    assert(latency.socket_descriptor == -1);
    assert(strlen(error) > 0U);
}

static void test_close_is_idempotent(void)
{
    struct sqm_mon_latency latency;

    latency_init(&latency);
    latency_close(&latency);
    latency_close(&latency);

    assert(latency.socket_descriptor == -1);
}

int main(void)
{
    test_initial_state_is_closed();
    test_invalid_target_is_rejected_before_opening_socket();
    test_close_is_idempotent();

    (void)puts("latency tests passed");
    return 0;
}
