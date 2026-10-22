#ifndef LATENCY_ICMP_H_INCLUDED
#define LATENCY_ICMP_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* Linux's fixed time64 socket option; older musl headers expose only SO_TIMESTAMPNS. */
#ifndef SO_TIMESTAMPNS_NEW
#define SO_TIMESTAMPNS_NEW 64
#endif

/* Echo requests (IPv4 or IPv6), or IPv4 timestamp requests (types 13 and 14). */
enum icmp_mode {
	ICMP_MODE_ECHO,
	ICMP_MODE_TIMESTAMP
};

/* Request sizes: echo carries the session token and serial in 56 data bytes. */
#define ICMP_ECHO_REQUEST_BYTES 64U
#define ICMP_TIMESTAMP_REQUEST_BYTES 20U

/* What icmp_receive() found on the socket. */
enum icmp_receive_result {
	ICMP_RECEIVE_ERROR = -1,
	ICMP_RECEIVE_NONE,
	ICMP_RECEIVE_REPLY,
	ICMP_RECEIVE_IGNORED
};

struct icmp_address {
	struct sockaddr_storage address;
	socklen_t length;
};

struct icmp_socket {
	int fd;
	int family;
	uint16_t identifier;
	bool timestamp_enabled;
};

struct icmp_reply {
	struct icmp_address source;
	uint16_t sequence;
	uint64_t token;
	uint64_t serial;
	uint32_t originate_ms;
	uint32_t receive_ms;
	uint32_t transmit_ms;
	uint64_t realtime_us;
	bool kernel_timestamp;
};

/* Native probes currently use numeric addresses, without blocking DNS. */
int icmp_address_parse(struct icmp_address *address, const char *target);
bool icmp_address_equal(const struct icmp_address *first, const struct icmp_address *second);
uint16_t icmp_checksum(const unsigned char *packet, size_t length);
size_t icmp_build(
	unsigned char packet[ICMP_ECHO_REQUEST_BYTES],
	int family,
	enum icmp_mode mode,
	uint16_t identifier,
	uint16_t sequence,
	uint64_t token,
	uint64_t serial,
	uint32_t originate_ms
);
bool icmp_parse(
	const unsigned char *packet,
	size_t length,
	int family,
	enum icmp_mode mode,
	uint16_t identifier,
	struct icmp_reply *reply
);
bool icmp_timestamp(const struct msghdr *message, uint64_t *realtime_us);
int icmp_open(
	struct icmp_socket *channel,
	int family,
	enum icmp_mode mode,
	const char *interface,
	uint16_t identifier,
	char *error,
	size_t error_size
);
void icmp_close(struct icmp_socket *channel);
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
);
/* Reads one datagram; an error is a socket or clock failure. */
enum icmp_receive_result icmp_receive(
	const struct icmp_socket *channel,
	enum icmp_mode mode,
	struct icmp_reply *reply,
	char *error,
	size_t error_size
);

#endif
