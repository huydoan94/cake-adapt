#define _GNU_SOURCE

#include "latency/icmp.h"
#include "common/constants.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* linux/icmp.h's raw-socket type filter, at SOL_RAW; that header conflicts with netinet's. */
#define LINUX_ICMP_FILTER 1
#define IPV4_HEADER_MIN_BYTES 20U
/* Type, code, checksum, identifier, sequence, then the token and serial. */
#define ICMP_ECHO_REPLY_MIN_BYTES 24U
/* IPv4 flags and fragment offset: any set bit other than DF means a fragment. */
#define IPV4_FRAGMENT_MASK 0x3fffU

static void put_integer(unsigned char *bytes, uint64_t value, size_t length)
{
	for (size_t index = length; index > 0U; index--) {
		bytes[index - 1U] = (unsigned char)value;
		value >>= 8U;
	}
}

static uint64_t get_integer(const unsigned char *bytes, size_t length)
{
	uint64_t value = 0U;

	for (size_t index = 0U; index < length; index++)
		value = (value << 8U) | bytes[index];
	return value;
}

int icmp_address_parse(struct icmp_address *address, const char *target)
{
	struct sockaddr_in *ipv4;
	struct sockaddr_in6 *ipv6;

	*address = (struct icmp_address){ 0 };
	ipv4 = (struct sockaddr_in *)&address->address;
	if (inet_pton(AF_INET, target, &ipv4->sin_addr) == 1) {
		ipv4->sin_family = AF_INET;
		address->length = sizeof(*ipv4);
		return 0;
	}
	ipv6 = (struct sockaddr_in6 *)&address->address;
	if (inet_pton(AF_INET6, target, &ipv6->sin6_addr) == 1) {
		ipv6->sin6_family = AF_INET6;
		address->length = sizeof(*ipv6);
		return 0;
	}
	return -1;
}

bool icmp_address_equal(const struct icmp_address *first, const struct icmp_address *second)
{
	const struct sockaddr_storage *first_storage = &first->address;
	const struct sockaddr_storage *second_storage = &second->address;

	if (first_storage->ss_family != second_storage->ss_family)
		return false;
	if (first_storage->ss_family == AF_INET) {
		const struct sockaddr_in *a = (const struct sockaddr_in *)first_storage;
		const struct sockaddr_in *b = (const struct sockaddr_in *)second_storage;
		const struct in_addr *a_address = &a->sin_addr;
		const struct in_addr *b_address = &b->sin_addr;

		return a_address->s_addr == b_address->s_addr;
	}
	if (first_storage->ss_family == AF_INET6) {
		const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)first_storage;
		const struct sockaddr_in6 *b = (const struct sockaddr_in6 *)second_storage;

		return a->sin6_scope_id == b->sin6_scope_id &&
		       memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr)) == 0;
	}
	return false;
}

uint16_t icmp_checksum(const unsigned char *packet, size_t length)
{
	uint32_t sum = 0U;

	for (size_t index = 0U; index < length; index += 2U)
		sum += ((uint32_t)packet[index] << 8U) |
		       (index + 1U < length ? packet[index + 1U] : 0U);
	while (sum >> 16U)
		sum = (sum & UINT16_MAX) + (sum >> 16U);
	return (uint16_t)~sum;
}

size_t icmp_build(
	unsigned char packet[ICMP_ECHO_REQUEST_BYTES],
	int family,
	enum icmp_mode mode,
	uint16_t identifier,
	uint16_t sequence,
	uint64_t token,
	uint64_t serial,
	uint32_t originate_ms
)
{
	size_t length = mode == ICMP_MODE_TIMESTAMP ? ICMP_TIMESTAMP_REQUEST_BYTES :
						      ICMP_ECHO_REQUEST_BYTES;

	memset(packet, 0, length);
	packet[0] = mode == ICMP_MODE_TIMESTAMP ?
			    ICMP_TIMESTAMP :
			    (family == AF_INET ? ICMP_ECHO : ICMP6_ECHO_REQUEST);
	put_integer(packet + 4U, identifier, 2U);
	put_integer(packet + 6U, sequence, 2U);
	if (mode == ICMP_MODE_TIMESTAMP) {
		put_integer(packet + 8U, originate_ms, 4U);
	} else {
		put_integer(packet + 8U, token, 8U);
		put_integer(packet + 16U, serial, 8U);
	}
	/* The IPv6 raw socket computes the checksum with its pseudoheader. */
	if (family == AF_INET)
		put_integer(packet + 2U, icmp_checksum(packet, length), 2U);
	return length;
}

bool icmp_parse(
	const unsigned char *packet,
	size_t length,
	int family,
	enum icmp_mode mode,
	uint16_t identifier,
	struct icmp_reply *reply
)
{
	size_t header_bytes = 0U;
	unsigned char type = mode == ICMP_MODE_TIMESTAMP ?
				     ICMP_TIMESTAMPREPLY :
				     (family == AF_INET ? ICMP_ECHOREPLY : ICMP6_ECHO_REPLY);

	/* Raw IPv4 delivers the IP header: version 4, ICMP, whole length, not a fragment. */
	if (family == AF_INET) {
		if (length < IPV4_HEADER_MIN_BYTES || packet[0] >> 4U != 4U ||
		    packet[9] != IPPROTO_ICMP)
			return false;
		header_bytes = (size_t)(packet[0] & 15U) * 4U;
		if (header_bytes < IPV4_HEADER_MIN_BYTES || header_bytes > length ||
		    get_integer(packet + 2U, 2U) != length ||
		    (get_integer(packet + 6U, 2U) & IPV4_FRAGMENT_MASK) != 0U)
			return false;
	} else if (family != AF_INET6 || mode == ICMP_MODE_TIMESTAMP) {
		return false;
	}
	packet += header_bytes;
	length -= header_bytes;
	if (length < (mode == ICMP_MODE_TIMESTAMP ? ICMP_TIMESTAMP_REQUEST_BYTES :
						    ICMP_ECHO_REPLY_MIN_BYTES) ||
	    packet[0] != type || packet[1] != 0U || get_integer(packet + 4U, 2U) != identifier ||
	    (family == AF_INET && icmp_checksum(packet, length) != 0U))
		return false;
	reply->sequence = (uint16_t)get_integer(packet + 6U, 2U);
	if (mode == ICMP_MODE_TIMESTAMP) {
		reply->originate_ms = (uint32_t)get_integer(packet + 8U, 4U);
		reply->receive_ms = (uint32_t)get_integer(packet + 12U, 4U);
		reply->transmit_ms = (uint32_t)get_integer(packet + 16U, 4U);
	} else {
		reply->token = get_integer(packet + 8U, 8U);
		reply->serial = get_integer(packet + 16U, 8U);
	}
	return true;
}

/* musl's i386 CMSG_NXTHDR uses a signed alignment mask inside the libc macro. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
static struct cmsghdr *next_control(struct msghdr *message, struct cmsghdr *control)
{
	return CMSG_NXTHDR(message, control);
}
#pragma GCC diagnostic pop

bool icmp_timestamp(const struct msghdr *message, uint64_t *realtime_us)
{
	struct msghdr copy = *message;
	struct cmsghdr *control;

	if ((message->msg_flags & MSG_CTRUNC) != 0)
		return false;
	for (control = CMSG_FIRSTHDR(&copy); control != NULL;
	     control = next_control(&copy, control)) {
		/* Fixed kernel ABI on i386, x86_64 and arm64, independent of libc time_t. */
		int64_t timestamp[2];

		if (control->cmsg_len < CMSG_LEN(0U) ||
		    control->cmsg_len >
			    copy.msg_controllen - (size_t)((unsigned char *)control -
							   (unsigned char *)copy.msg_control))
			return false;
		if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SO_TIMESTAMPNS_NEW ||
		    control->cmsg_len != CMSG_LEN(sizeof(timestamp)))
			continue;
		memcpy(timestamp, CMSG_DATA(control), sizeof(timestamp));
		if (timestamp[0] < 0 || timestamp[1] < 0 ||
		    timestamp[1] >= (int64_t)NANOSECONDS_PER_SECOND ||
		    (uint64_t)timestamp[0] > UINT64_MAX / MICROSECONDS_PER_SECOND - 1U)
			return false;
		*realtime_us = (uint64_t)timestamp[0] * MICROSECONDS_PER_SECOND +
			       ns_to_us((uint64_t)timestamp[1]);
		return true;
	}
	return false;
}

static int socket_filter(int descriptor, int family, uint16_t identifier)
{
	/* Raw IPv4 includes its variable IP header, raw IPv6 starts at ICMPv6. */
	struct sock_filter ipv4[] = {
		BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, 0U),
		BPF_STMT(BPF_LD | BPF_H | BPF_IND, 4U),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, identifier, 0U, 1U),
		BPF_STMT(BPF_RET | BPF_K, UINT32_MAX),
		BPF_STMT(BPF_RET | BPF_K, 0U),
	};
	struct sock_filter ipv6[] = {
		BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4U),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, identifier, 0U, 1U),
		BPF_STMT(BPF_RET | BPF_K, UINT32_MAX),
		BPF_STMT(BPF_RET | BPF_K, 0U),
	};
	struct sock_fprog program = {
		.len = (unsigned short)(family == AF_INET ? ARRAY_SIZE(ipv4) : ARRAY_SIZE(ipv6)),
		.filter = family == AF_INET ? ipv4 : ipv6,
	};

	return setsockopt(descriptor, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program));
}

int icmp_open(
	struct icmp_socket *channel,
	int family,
	enum icmp_mode mode,
	const char *interface,
	uint16_t identifier,
	char *error,
	size_t error_size
)
{
	int enabled = 1;

	*channel = (struct icmp_socket){ .fd = -1, .family = family, .identifier = identifier };
	if ((family != AF_INET && family != AF_INET6) ||
	    (family == AF_INET6 && mode == ICMP_MODE_TIMESTAMP))
		return error_set(error, error_size, "invalid ICMP socket family/mode");
	channel->fd =
		socket(family,
		       SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
		       family == AF_INET ? IPPROTO_ICMP : IPPROTO_ICMPV6);
	if (channel->fd < 0)
		return error_set(
			error,
			error_size,
			"could not open ICMP socket: %s",
			strerror(errno)
		);
	if (setsockopt(
		    channel->fd,
		    SOL_SOCKET,
		    SO_BINDTODEVICE,
		    interface,
		    (socklen_t)(strlen(interface) + 1U)
	    ) != 0 ||
	    socket_filter(channel->fd, family, identifier) != 0)
		goto failed;
	if (family == AF_INET) {
		/* A set bit drops that type: only the expected reply passes. */
		uint32_t filter =
			~(UINT32_C(1)
			  << (mode == ICMP_MODE_TIMESTAMP ? ICMP_TIMESTAMPREPLY : ICMP_ECHOREPLY));

		if (setsockopt(channel->fd, SOL_RAW, LINUX_ICMP_FILTER, &filter, sizeof(filter)) !=
		    0)
			goto failed;
	} else {
		struct icmp6_filter filter;

		ICMP6_FILTER_SETBLOCKALL(&filter);
		filter.icmp6_filt[ICMP6_ECHO_REPLY >> 5U] &=
			~(UINT32_C(1) << (ICMP6_ECHO_REPLY & 31U));
		if (setsockopt(channel->fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter)) !=
		    0)
			goto failed;
	}
	channel->timestamp_enabled =
		setsockopt(channel->fd, SOL_SOCKET, SO_TIMESTAMPNS_NEW, &enabled, sizeof(enabled)) ==
		0;
	return 0;
failed:
	(void)error_set(error, error_size, "could not configure ICMP socket: %s", strerror(errno));
	icmp_close(channel);
	return -1;
}

void icmp_close(struct icmp_socket *channel)
{
	if (channel->fd >= 0)
		(void)close(channel->fd);
	channel->fd = -1;
}

int icmp_send(
	const struct icmp_socket *channel,
	const struct icmp_address *address,
	enum icmp_mode mode,
	uint16_t sequence,
	uint64_t token,
	uint64_t serial,
	uint32_t originate_ms,
	uint64_t *sent_us,
	char *error,
	size_t error_size
)
{
	unsigned char packet[ICMP_ECHO_REQUEST_BYTES];
	size_t length = icmp_build(
		packet,
		channel->family,
		mode,
		channel->identifier,
		sequence,
		token,
		serial,
		originate_ms
	);
	ssize_t sent;

	/* Exclude packet preparation and event-loop delay from the measured RTT. */
	if (!read_clock_us(CLOCK_MONOTONIC, sent_us))
		return error_set(error, error_size, "ICMP send clock failed: %s", strerror(errno));
	sent =
		sendto(channel->fd,
		       packet,
		       length,
		       0,
		       (const struct sockaddr *)&address->address,
		       address->length);

	if (sent != (ssize_t)length)
		return error_set(
			error,
			error_size,
			"ICMP send failed: %s",
			sent < 0 ? strerror(errno) : "short datagram"
		);
	return 0;
}

enum icmp_receive_result icmp_receive(
	const struct icmp_socket *channel,
	enum icmp_mode mode,
	struct icmp_reply *reply,
	char *error,
	size_t error_size
)
{
	unsigned char packet[2048];
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int64_t) * 2U)];
	} ancillary;
	struct iovec vector = { .iov_base = packet, .iov_len = sizeof(packet) };
	struct icmp_address *source = &reply->source;
	const struct sockaddr_storage *source_storage = &source->address;
	struct msghdr message;
	ssize_t length;

	*reply = (struct icmp_reply){ 0 };
	message = (struct msghdr){ .msg_name = &source->address,
				   .msg_namelen = sizeof(source->address),
				   .msg_iov = &vector,
				   .msg_iovlen = 1U,
				   .msg_control = ancillary.bytes,
				   .msg_controllen = sizeof(ancillary.bytes) };
	length = recvmsg(channel->fd, &message, MSG_DONTWAIT);
	if (length < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return ICMP_RECEIVE_NONE;
		if (errno == EINTR)
			return ICMP_RECEIVE_IGNORED;
		(void)error_set(error, error_size, "ICMP receive failed: %s", strerror(errno));
		return ICMP_RECEIVE_ERROR;
	}
	if ((message.msg_flags & MSG_TRUNC) != 0 ||
	    !icmp_parse(packet, (size_t)length, channel->family, mode, channel->identifier, reply))
		return ICMP_RECEIVE_IGNORED;
	source->length = message.msg_namelen;
	if (source_storage->ss_family != channel->family)
		return ICMP_RECEIVE_IGNORED;
	reply->kernel_timestamp = icmp_timestamp(&message, &reply->realtime_us);
	if (!reply->kernel_timestamp && !read_clock_us(CLOCK_REALTIME, &reply->realtime_us)) {
		(void)error_set(error, error_size, "ICMP receive clock failed: %s", strerror(errno));
		return ICMP_RECEIVE_ERROR;
	}
	return ICMP_RECEIVE_REPLY;
}
