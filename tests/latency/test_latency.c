#define _POSIX_C_SOURCE 200809L

#include "latency/latency.h"
#include "common/constants.h"

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_initial_state_is_closed(void)
{
    struct latency latency;

    latency_init(&latency);

    assert(latency.children[0].output_descriptor == -1);
    assert(latency.children[0].process_identifier == -1);
    assert(latency_child_count(&latency) == 0U);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(!latency_is_open(&latency));
}

static void test_receive_buffered_output(void)
{
    struct latency latency;
    struct latency_sample sample;
    int descriptors[2];
    char error[256] = "";
    const char first[] = "[123.456000] 1.1.1.1 : [1], 64 bytes, ";
    const char rest[] = "2.50 ms\r\n[123.756000] 1.1.1.1 : [2], timed out\n";

    latency_init(&latency);
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    latency.children[0].output_descriptor = descriptors[0];
    /* This fixture owns only a pipe, not an fping child. */
    latency.children[0].process_identifier = getpid();
    latency.child_count = 1U;

    assert(latency_receive_child(
        &latency,
        1U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_ERROR);
    assert(strstr(error, "not running") != NULL);

    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_PENDING);
    assert(write(descriptors[1], first, sizeof(first) - 1U) == (ssize_t)(sizeof(first) - 1U));
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_PENDING);
    assert(latency.children[0].output_length == sizeof(first) - 1U);
    assert(write(descriptors[1], rest, sizeof(rest) - 1U) == (ssize_t)(sizeof(rest) - 1U));
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_SUCCESS);
    assert(sample.sequence == 1U);
    assert(sample.download_owd_microseconds == 1250U);
    assert(sample.upload_owd_microseconds == 1250U);
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_TIMEOUT);
    assert(sample.sequence == 2U);
    assert(latency.children[0].output_length == 0U);

    assert(write(descriptors[1], "bad\n", 4U) == 4);
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_ERROR);
    assert(strstr(error, "unexpected fping output") != NULL);

    memset(latency.children[0].output_buffer, 'x', sizeof(latency.children[0].output_buffer));
    latency.children[0].output_length = sizeof(latency.children[0].output_buffer);
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_ERROR);
    assert(strstr(error, "too long") != NULL);
    latency.children[0].output_length = 0U;

    assert(close(descriptors[1]) == 0);
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_ERROR);
    assert(strstr(error, "output closed") != NULL);
    assert(close(descriptors[0]) == 0);
}

static void test_invalid_target_is_rejected_before_starting_fping(void)
{
    struct latency latency;
    const char *targets[] = { "not an endpoint" };
    char error[256] = "";

    latency_init(&latency);

    assert(latency_open(
        &latency,
        "lo",
        targets,
        1U,
        1000000U,
        "",
        "",
        error,
        sizeof(error)
    ) != 0);
    assert(latency.children[0].output_descriptor == -1);
    assert(latency.children[0].process_identifier == -1);
    assert(strlen(error) > 0U);
}

static void test_empty_target_list_is_rejected(void)
{
    struct latency latency;
    char error[256] = "";

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        NULL,
        0U,
        1000000U,
        "",
        "",
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "at least one target") != NULL);
    assert(!latency_is_open(&latency));
}

static void test_sub_millisecond_response_spacing_is_rejected(void)
{
    struct latency latency;
    const char *targets[] = { "1.1.1.1", "9.9.9.9" };
    char error[256] = "";

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        targets,
        2U,
        1999U,
        "",
        "",
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "at least 1 ms per target") != NULL);
    assert(!latency_is_open(&latency));
}

static void test_close_is_idempotent(void)
{
    struct latency latency;

    latency_init(&latency);
    latency_close(&latency);
    latency_close(&latency);

    assert(latency.children[0].output_descriptor == -1);
    assert(latency.children[0].process_identifier == -1);
}

static void test_close_releases_every_owned_descriptor(void)
{
    struct latency latency;
    int first[2];
    int second[2];

    latency_init(&latency);
    assert(pipe(first) == 0);
    assert(pipe(second) == 0);
    latency.children[0].output_descriptor = first[0];
    latency.children[1].output_descriptor = second[0];
    latency.child_count = 2U;
    latency_close(&latency);
    assert(fcntl(first[0], F_GETFD) == -1);
    assert(errno == EBADF);
    assert(fcntl(second[0], F_GETFD) == -1);
    assert(errno == EBADF);
    assert(latency_child_count(&latency) == 0U);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(latency_child_descriptor(&latency, CONFIG_MAX_REFLECTORS) == -1);
    assert(close(first[1]) == 0);
    assert(close(second[1]) == 0);
}

static void test_irtt_receive_ignores_non_sample_lines(void)
{
    struct latency latency;
    struct latency_sample sample;
    const char output[] = "IRTT client\nseq=7 rtt=3ms rd=1ms sd=2ms\n";
    char error[256] = "";
    int descriptors[2];

    latency_init(&latency);
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    latency.backend = LATENCY_BACKEND_IRTT;
    latency.active = true;
    latency.child_count = 1U;
    latency.children[0].output_descriptor = descriptors[0];
    latency.children[0].process_identifier = getpid();
    latency.children[0].target = "9.9.9.9";
    assert(write(descriptors[1], output, sizeof(output) - 1U) ==
        (ssize_t)(sizeof(output) - 1U));
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_SUCCESS);
    assert(strcmp(sample.target, "9.9.9.9") == 0);
    assert(sample.sequence == 7U);
    assert(sample.download_owd_microseconds == 1000);
    assert(sample.upload_owd_microseconds == 2000);
    assert(sample.timestamp_microseconds > 0U);
    assert(close(descriptors[1]) == 0);
    assert(close(descriptors[0]) == 0);
}

static void test_pinger_arguments_reject_command_substitution(void)
{
    struct latency latency;
    const char *targets[] = { "1.1.1.1" };
    char error[256] = "";

    latency_init(&latency);
    assert(latency_open(&latency, "lo", targets, 1U, 1000000U, "$(id)", "", error, sizeof(error)) != 0);
    assert(strstr(error, "ping_extra_args") != NULL);
    assert(!latency_is_open(&latency));
    assert(latency_open(
        &latency,
        "lo",
        targets,
        1U,
        1000000U,
        "",
        "'unterminated",
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "ping_prefix_string") != NULL);
    assert(!latency_is_open(&latency));
}

static size_t open_descriptor_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    size_t count = 0U;

    assert(directory != NULL);
    while (readdir(directory) != NULL) {
        count++;
    }
    assert(closedir(directory) == 0);
    return count;
}

static void test_failed_spawn_closes_pipe(void)
{
    struct latency latency;
    const char *targets[] = { "127.0.0.1" };
    char error[256];
    size_t descriptors = open_descriptor_count();

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        targets,
        1U,
        1000000U,
        "",
        "/nonexistent-cake-adapt-test/fping",
        error,
        sizeof(error)
    ) != 0);
    assert(strstr(error, "could not start fping") != NULL);
    assert(!latency_is_open(&latency));
    assert(open_descriptor_count() == descriptors);
}

static void test_prefix_and_extra_args_reach_owned_process(void)
{
    struct latency latency;
    const char *targets[] = { "1.1.1.1", "::1" };
    char error[256] = "";
    char output[1024];
    size_t length = 0U;
    struct pollfd descriptor;

    latency_init(&latency);
    assert(latency_open(
        &latency,
        "lo",
        targets,
        2U,
        300000U,
        "-I 'lo2' -k 768",
        "/usr/bin/printf '%s\\n'",
        error,
        sizeof(error)
    ) == 0);
    descriptor = (struct pollfd) { .fd = latency.children[0].output_descriptor, .events = POLLIN };
    for (;;) {
        ssize_t bytes;

        assert(poll(&descriptor, 1U, 1000) > 0);
        assert(length < sizeof(output) - 1U);
        bytes = read(latency.children[0].output_descriptor, output + length, sizeof(output) - 1U - length);
        assert(bytes >= 0);
        if (bytes == 0) {
            break;
        }
        length += (size_t)bytes;
    }
    output[length] = '\0';
    assert(strcmp(
        output,
        "/usr/bin/fping\n-I\nlo2\n-k\n768\n--timestamp\n--loop\n"
        "--period\n300\n--interval\n150\n--timeout\n10000\n1.1.1.1\n::1\n"
    ) == 0);
    latency_close(&latency);
    assert(!latency_is_open(&latency));
    assert(latency_child_count(&latency) == 0U);
    assert(target_is_valid("::1"));
    assert(target_is_valid("2001:4860:4860::8888"));
}

static void test_irtt_children_start_in_separate_slots(void)
{
    struct latency latency;
    struct latency_sample sample;
    const char *targets[] = { "1.1.1.1", "2001:db8::1" };
    char error[256] = "";
    char output[1024];
    size_t length = 0U;
    struct pollfd descriptor;

    latency_init(&latency);
    assert(latency_open_irtt(
        &latency,
        targets,
        2U,
        300U * MILLISECOND,
        10U,
        "--fill=rand",
        "/usr/bin/printf '%s\\n'",
        1000U,
        error,
        sizeof(error)
    ) == 0);
    assert(latency_is_open(&latency));
    assert(latency_child_count(&latency) == 2U);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(latency_child_descriptor(&latency, 1U) == -1);
    assert(latency_irtt_start_pending(&latency));
    assert(latency_start_irtt_children(
        &latency,
        999U,
        error,
        sizeof(error)
    ) == 0);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(latency_child_descriptor(&latency, 1U) == -1);
    assert(latency_start_irtt_children(
        &latency,
        1000U,
        error,
        sizeof(error)
    ) == 0);
    assert(latency_child_descriptor(&latency, 0U) >= 0);
    assert(latency_child_descriptor(&latency, 1U) == -1);
    assert(latency_irtt_next_start_microseconds(&latency) == 151000U);

    descriptor = (struct pollfd) {
        .fd = latency_child_descriptor(&latency, 0U),
        .events = POLLIN
    };
    for (;;) {
        ssize_t bytes;

        assert(poll(&descriptor, 1U, 1000) > 0);
        bytes = read(
            descriptor.fd,
            output + length,
            sizeof(output) - 1U - length
        );
        assert(bytes >= 0);
        if (bytes == 0) {
            break;
        }
        length += (size_t)bytes;
    }
    output[length] = '\0';
    assert(strcmp(
        output,
        "/usr/bin/irtt\nclient\n--fill=rand\n-i\n0.300000s\n"
            "-d\n10m\n1.1.1.1\n"
    ) == 0);

    assert(latency_start_irtt_children(
        &latency,
        151000U,
        error,
        sizeof(error)
    ) == 0);
    assert(latency_child_descriptor(&latency, 1U) >= 0);
    assert(!latency_irtt_start_pending(&latency));
    assert(latency_receive_child(
        &latency,
        0U,
        &sample,
        error,
        sizeof(error)
    ) == LATENCY_PROBE_RESTART);
    assert(strstr(error, "irtt exited with status 0") != NULL);
    assert(latency_child_descriptor(&latency, 0U) == -1);
    assert(latency_child_descriptor(&latency, 1U) >= 0);
    assert(latency_irtt_start_pending(&latency));
    latency_close(&latency);
    assert(!latency_is_open(&latency));
}

int main(void)
{
    test_initial_state_is_closed();
    test_receive_buffered_output();
    test_invalid_target_is_rejected_before_starting_fping();
    test_empty_target_list_is_rejected();
    test_sub_millisecond_response_spacing_is_rejected();
    test_close_is_idempotent();
    test_close_releases_every_owned_descriptor();
    test_irtt_receive_ignores_non_sample_lines();
    test_pinger_arguments_reject_command_substitution();
    test_failed_spawn_closes_pipe();
    test_prefix_and_extra_args_reach_owned_process();
    test_irtt_children_start_in_separate_slots();

    (void)puts("latency tests passed");
    return 0;
}
