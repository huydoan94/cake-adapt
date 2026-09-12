#define _GNU_SOURCE

#include "latency.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LATENCY_RESPONSE_SIZE 256U

static void set_error(
    char *error,
    size_t error_size,
    const char *format,
    ...
)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint16_t checksum(
    const void *data,
    size_t length
)
{
    const unsigned char *bytes = data;
    uint32_t sum = 0U;

    while (length >= sizeof(uint16_t)) {
        uint16_t word;

        (void)memcpy(&word, bytes, sizeof(word));
        sum += word;
        bytes += sizeof(word);
        length -= sizeof(word);
    }

    if (length > 0U) {
        sum += *bytes;
    }

    while ((sum >> 16U) != 0U) {
        sum = (sum & UINT16_MAX) + (sum >> 16U);
    }

    return (uint16_t)~sum;
}

static bool elapsed_microseconds(
    const struct timespec *start,
    const struct timespec *end,
    uint64_t *elapsed
)
{
    time_t seconds;
    long nanoseconds;

    seconds = end->tv_sec - start->tv_sec;
    nanoseconds = end->tv_nsec - start->tv_nsec;

    if (nanoseconds < 0L) {
        --seconds;
        nanoseconds += 1000000000L;
    }

    if (seconds < 0 ||
        (uint64_t)seconds > UINT64_MAX / 1000000U) {
        return false;
    }

    *elapsed = (uint64_t)seconds * 1000000U +
        (uint64_t)nanoseconds / 1000U;
    return true;
}

static int remaining_timeout(
    const struct timespec *start,
    int timeout_milliseconds
)
{
    struct timespec current;
    uint64_t elapsed;
    uint64_t timeout_microseconds;
    uint64_t remaining_microseconds;

    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0 ||
        !elapsed_microseconds(start, &current, &elapsed)) {
        return 0;
    }

    timeout_microseconds = (uint64_t)timeout_milliseconds * 1000U;
    if (elapsed >= timeout_microseconds) {
        return 0;
    }

    remaining_microseconds = timeout_microseconds - elapsed;
    return (int)((remaining_microseconds + 999U) / 1000U);
}

static bool response_matches(
    const unsigned char *response,
    size_t response_size,
    const struct sockaddr_in *source,
    const struct sqm_mon_latency *latency,
    uint16_t sequence
)
{
    const struct icmphdr *icmp_header;
    const struct iphdr *ip_header;
    size_t ip_header_size;

    if (source->sin_addr.s_addr != latency->target_address.sin_addr.s_addr ||
        response_size < sizeof(struct iphdr)) {
        return false;
    }

    ip_header = (const struct iphdr *)(const void *)response;
    if (ip_header->version != 4U) {
        return false;
    }

    ip_header_size = (size_t)ip_header->ihl * 4U;
    if (ip_header_size < sizeof(struct iphdr) ||
        response_size < ip_header_size + sizeof(struct icmphdr)) {
        return false;
    }

    icmp_header = (const struct icmphdr *)(const void *)(
        response + ip_header_size
    );
    return icmp_header->type == ICMP_ECHOREPLY &&
        icmp_header->code == 0U &&
        ntohs(icmp_header->un.echo.id) == latency->identifier &&
        ntohs(icmp_header->un.echo.sequence) == sequence;
}

static enum latency_probe_result receive_response(
    struct sqm_mon_latency *latency,
    uint16_t sequence,
    const struct timespec *sent_at,
    int timeout_milliseconds,
    struct latency_sample *sample,
    char *error,
    size_t error_size
)
{
    struct pollfd descriptor = {
        .fd = latency->socket_descriptor,
        .events = POLLIN,
        .revents = 0
    };
    union {
        long double alignment;
        unsigned char bytes[LATENCY_RESPONSE_SIZE];
    } response;

    for (;;) {
        struct sockaddr_in source;
        struct timespec received_at;
        socklen_t source_size = sizeof(source);
        ssize_t received_size;
        uint64_t elapsed;
        int poll_result;
        int timeout;

        timeout = remaining_timeout(sent_at, timeout_milliseconds);
        if (timeout == 0) {
            return LATENCY_PROBE_TIMEOUT;
        }

        poll_result = poll(&descriptor, 1U, timeout);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            set_error(
                error,
                error_size,
                "could not wait for ICMP response: %s",
                strerror(errno)
            );
            return LATENCY_PROBE_ERROR;
        }
        if (poll_result == 0) {
            return LATENCY_PROBE_TIMEOUT;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            set_error(
                error,
                error_size,
                "ICMP socket reported an error (revents=0x%x)",
                (unsigned int)descriptor.revents
            );
            return LATENCY_PROBE_ERROR;
        }

        received_size = recvfrom(
            latency->socket_descriptor,
            response.bytes,
            sizeof(response.bytes),
            0,
            (struct sockaddr *)(void *)&source,
            &source_size
        );
        if (received_size < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }

            set_error(
                error,
                error_size,
                "could not receive ICMP response: %s",
                strerror(errno)
            );
            return LATENCY_PROBE_ERROR;
        }

        if (!response_matches(
                response.bytes,
                (size_t)received_size,
                &source,
                latency,
                sequence
            )) {
            continue;
        }

        if (clock_gettime(CLOCK_MONOTONIC, &received_at) != 0 ||
            !elapsed_microseconds(sent_at, &received_at, &elapsed)) {
            set_error(error, error_size, "could not calculate probe latency");
            return LATENCY_PROBE_ERROR;
        }

        if (elapsed > UINT32_MAX) {
            sample->round_trip_microseconds = UINT32_MAX;
        } else {
            sample->round_trip_microseconds = (uint32_t)elapsed;
        }
        return LATENCY_PROBE_SUCCESS;
    }
}

void latency_init(struct sqm_mon_latency *latency)
{
    *latency = (struct sqm_mon_latency) {
        .socket_descriptor = -1,
        .target_address = {
            .sin_family = AF_INET,
            .sin_port = 0,
            .sin_addr = {
                .s_addr = INADDR_ANY
            }
        },
        .identifier = 0U,
        .next_sequence = 0U
    };
}

int latency_open(
    struct sqm_mon_latency *latency,
    const char *interface,
    const char *target,
    char *error,
    size_t error_size
)
{
    struct sockaddr_in target_address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = {
            .s_addr = INADDR_ANY
        }
    };
    int descriptor;

    if (inet_pton(AF_INET, target, &target_address.sin_addr) != 1) {
        set_error(
            error,
            error_size,
            "latency target '%s' is not an IPv4 address",
            target
        );
        return -1;
    }

    descriptor = socket(
        AF_INET,
        SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
        IPPROTO_ICMP
    );
    if (descriptor < 0) {
        set_error(
            error,
            error_size,
            "could not open ICMP socket: %s",
            strerror(errno)
        );
        return -1;
    }

    if (setsockopt(
            descriptor,
            SOL_SOCKET,
            SO_BINDTODEVICE,
            interface,
            (socklen_t)(strlen(interface) + 1U)
        ) != 0) {
        set_error(
            error,
            error_size,
            "could not bind ICMP socket to %s: %s",
            interface,
            strerror(errno)
        );
        (void)close(descriptor);
        return -1;
    }

    latency->socket_descriptor = descriptor;
    latency->target_address = target_address;
    latency->identifier = (uint16_t)((unsigned int)getpid() & UINT16_MAX);
    latency->next_sequence = 0U;
    return 0;
}

void latency_close(struct sqm_mon_latency *latency)
{
    if (latency->socket_descriptor >= 0) {
        (void)close(latency->socket_descriptor);
        latency->socket_descriptor = -1;
    }
}

enum latency_probe_result latency_probe(
    struct sqm_mon_latency *latency,
    int timeout_milliseconds,
    struct latency_sample *sample,
    char *error,
    size_t error_size
)
{
    struct icmphdr request = {
        .type = ICMP_ECHO,
        .code = 0,
        .checksum = 0U,
        .un.echo = {
            .id = 0U,
            .sequence = 0U
        }
    };
    struct timespec sent_at;
    ssize_t sent_size;
    uint16_t sequence;

    if (timeout_milliseconds <= 0) {
        set_error(error, error_size, "latency timeout must be positive");
        return LATENCY_PROBE_ERROR;
    }

    sequence = latency->next_sequence;
    ++latency->next_sequence;
    request.un.echo.id = htons(latency->identifier);
    request.un.echo.sequence = htons(sequence);
    request.checksum = checksum(&request, sizeof(request));

    if (clock_gettime(CLOCK_MONOTONIC, &sent_at) != 0) {
        set_error(
            error,
            error_size,
            "could not read the monotonic clock: %s",
            strerror(errno)
        );
        return LATENCY_PROBE_ERROR;
    }

    sent_size = sendto(
        latency->socket_descriptor,
        &request,
        sizeof(request),
        0,
        (const struct sockaddr *)(const void *)&latency->target_address,
        sizeof(latency->target_address)
    );
    if (sent_size < 0 || (size_t)sent_size != sizeof(request)) {
        set_error(
            error,
            error_size,
            "could not send ICMP probe: %s",
            sent_size < 0 ? strerror(errno) : "incomplete send"
        );
        return LATENCY_PROBE_ERROR;
    }

    return receive_response(
        latency,
        sequence,
        &sent_at,
        timeout_milliseconds,
        sample,
        error,
        error_size
    );
}
