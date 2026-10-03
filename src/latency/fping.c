#define _GNU_SOURCE

#include "latency/latency.h"
#include "latency/pinger.h"
#include "common/constants.h"
#include "common/error.h"
#include "common/helpers.h"
#include "config/defaults.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wordexp.h>

int latency_open(
    struct latency *latency,
    const char *interface,
    const char *const *targets,
    size_t target_count,
    uint64_t reflector_ping_interval_microseconds,
    const char *extra_arguments,
    const char *prefix,
    bool icmp_timestamps,
    char *error,
    size_t error_size
)
{
    char period_milliseconds[32];
    char response_interval_milliseconds[32];
    char **arguments = NULL;
    wordexp_t extra_words = { 0 };
    wordexp_t prefix_words = { 0 };
    uint64_t period;
    uint64_t response_interval;
    size_t index;
    size_t cursor = 0U;
    bool interface_configured = false;
    int result = -1;

    if (
        interface == NULL ||
        interface[0] == '\0'
    ) {
        error_set(error, error_size, "fping interface is empty");
        return -1;
    }
    if (
        targets == NULL || target_count == 0U || extra_arguments == NULL ||
        prefix == NULL
    ) {
        error_set(error, error_size, "fping requires at least one target");
        return -1;
    }
    if (target_count > CONFIG_MAX_REFLECTORS) {
        error_set(
            error,
            error_size,
            "fping supports at most %u targets",
            CONFIG_MAX_REFLECTORS
        );
        return -1;
    }
    if (
        reflector_ping_interval_microseconds / target_count <
        MICROSECONDS_PER_MILLISECOND
    ) {
        error_set(
            error,
            error_size,
            "reflector ping interval must provide at least 1 ms per target"
        );
        return -1;
    }
    if (validate_targets(targets, target_count, error, error_size) != 0) {
        return -1;
    }

    period = rounded_divide(
        reflector_ping_interval_microseconds,
        MICROSECONDS_PER_MILLISECOND
    );
    response_interval =
        reflector_ping_interval_microseconds / target_count /
        MICROSECONDS_PER_MILLISECOND;
    (void)snprintf(
        period_milliseconds,
        sizeof(period_milliseconds),
        "%" PRIu64,
        period
    );
    (void)snprintf(
        response_interval_milliseconds,
        sizeof(response_interval_milliseconds),
        "%" PRIu64,
        response_interval
    );

    if (
        expand_words(
            extra_arguments,
            false,
            OPTION_PING_EXTRA_ARGS,
            &extra_words,
            error,
            error_size
        ) != 0 ||
        expand_words(
            prefix,
            true,
            OPTION_PING_PREFIX_STRING,
            &prefix_words,
            error,
            error_size
        ) != 0
    ) {
        goto done;
    }
    arguments = calloc(
        /* Up to 13 fixed fping arguments and the terminating NULL. */
        prefix_words.we_wordc + extra_words.we_wordc + target_count + 14U,
        sizeof(*arguments)
    );
    if (arguments == NULL) {
        error_set(
            error,
            error_size,
            "could not allocate fping arguments: %s",
            strerror(errno)
        );
        goto done;
    }
    for (index = 0U; index < prefix_words.we_wordc; index++) {
        arguments[cursor++] = prefix_words.we_wordv[index];
    }
    arguments[cursor++] = (char *)FPING_PATH;
    for (index = 0U; index < extra_words.we_wordc; index++) {
        arguments[cursor++] = extra_words.we_wordv[index];
        if (
            strncmp(
                extra_words.we_wordv[index],
                FPING_INTERFACE_SHORT,
                strlen(FPING_INTERFACE_SHORT)
            ) == 0 ||
            strcmp(extra_words.we_wordv[index], FPING_INTERFACE_LONG) == 0 ||
            strncmp(
                extra_words.we_wordv[index],
                FPING_INTERFACE_LONG_PREFIX,
                strlen(FPING_INTERFACE_LONG_PREFIX)
            ) == 0
        ) {
            interface_configured = true;
        }
    }
    /* Keep the SQM interface default, but honor an explicit routing override. */
    if (!interface_configured) {
        arguments[cursor++] = (char *)FPING_INTERFACE_SHORT;
        arguments[cursor++] = (char *)interface;
    }
    arguments[cursor++] = (char *)FPING_TIMESTAMP;
    arguments[cursor++] = (char *)FPING_LOOP;
    arguments[cursor++] = (char *)FPING_PERIOD;
    arguments[cursor++] = period_milliseconds;
    arguments[cursor++] = (char *)FPING_INTERVAL;
    arguments[cursor++] = response_interval_milliseconds;
    arguments[cursor++] = (char *)FPING_TIMEOUT;
    arguments[cursor++] = (char *)DEFAULT_FPING_TIMEOUT_MILLISECONDS;
    if (icmp_timestamps) {
        arguments[cursor++] = (char *)FPING_ICMP_TIMESTAMP;
    }
    for (index = 0U; index < target_count; index++) {
        arguments[cursor++] = (char *)targets[index];
    }

    if (
        start_child(
            &latency->children[0],
            arguments,
            PINGER_METHOD_FPING,
            error,
            error_size
        ) != 0
    ) {
        goto done;
    }
    latency->backend = icmp_timestamps ? LATENCY_BACKEND_FPING_TS : LATENCY_BACKEND_FPING;
    latency->active = true;
    latency->child_count = 1U;
    result = 0;

done:
    free(arguments);
    wordfree(&prefix_words);
    wordfree(&extra_words);
    return result;
}
