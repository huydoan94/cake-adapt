#define _GNU_SOURCE

#include "latency/icmp.h"
#include "constants/icmp.h"
#include "common/arith.h"

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The send path's system calls and clocks, faked: --wrap in tests/Makefile. */
ssize_t __wrap_sendto(
	int descriptor,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length
);
ssize_t __wrap_recvmsg(int descriptor, struct msghdr *message, int flags);
bool __wrap_read_clock_us(clockid_t identifier, uint64_t *timestamp_us);

/* Error-queue entries: a realtime send stamp each, 0 for an entry without one. */
static uint64_t queued_stamps_us[16];
static size_t queued_count;
static size_t queue_reads;
static uint64_t monotonic_reads_us[2];
static size_t monotonic_calls;

ssize_t __wrap_sendto(
	int descriptor,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length
)
{
	(void)descriptor;
	(void)buffer;
	(void)flags;
	(void)address;
	(void)address_length;
	return (ssize_t)length;
}

ssize_t __wrap_recvmsg(int descriptor, struct msghdr *message, int flags)
{
	struct cmsghdr *header;
	int64_t stamps[6] = { 0 };
	uint64_t stamp_us;

	(void)descriptor;
	assert((flags & MSG_ERRQUEUE) != 0);
	queue_reads++;
	if (queued_count == 0U) {
		errno = EAGAIN;
		return -1;
	}
	stamp_us = queued_stamps_us[0];
	memmove(queued_stamps_us,
		queued_stamps_us + 1U,
		--queued_count * sizeof(queued_stamps_us[0]));
	message->msg_flags = 0;
	header = CMSG_FIRSTHDR(message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = stamp_us != 0U ? 65 : 11;
	header->cmsg_len = CMSG_LEN(sizeof(stamps));
	stamps[0] = (int64_t)(stamp_us / 1000000U);
	stamps[1] = (int64_t)(stamp_us % 1000000U * 1000U);
	memcpy(CMSG_DATA(header), stamps, sizeof(stamps));
	message->msg_controllen = CMSG_SPACE(sizeof(stamps));
	return 0;
}

/* Monotonic: 1000 us before the send, 1900 after; realtime 50000 us after it. */
bool __wrap_read_clock_us(clockid_t identifier, uint64_t *timestamp_us)
{
	if (identifier == CLOCK_REALTIME)
		*timestamp_us = 50000U;
	else
		*timestamp_us = monotonic_reads_us[monotonic_calls++ % 2U];
	return true;
}

/* Type numbers stay literal here: an independent check of the header names icmp.c uses. */
static size_t ipv4_reply(unsigned char *packet, enum icmp_mode mode, size_t header_bytes)
{
	unsigned char request[ICMP_ECHO_REQUEST_BYTES];
	size_t length = icmp_build(
		request,
		AF_INET,
		mode,
		0x1234U,
		0xabcdU,
		UINT64_C(0x0102030405060708),
		UINT64_C(0x1112131415161718),
		1000U
	);
	uint16_t checksum;

	memset(packet, 0, 100U);
	packet[0] = (unsigned char)(0x40U | header_bytes / 4U);
	packet[2] = (unsigned char)((length + header_bytes) >> 8U);
	packet[3] = (unsigned char)(length + header_bytes);
	packet[9] = IPPROTO_ICMP;
	request[0] = mode == ICMP_MODE_TIMESTAMP ? 14U : 0U;
	request[2] = request[3] = 0U;
	checksum = icmp_checksum(request, length);
	request[2] = (unsigned char)(checksum >> 8U);
	request[3] = (unsigned char)checksum;
	memcpy(packet + header_bytes, request, length);
	return length + header_bytes;
}

static void test_wire(void)
{
	unsigned char packet[100];
	struct icmp_reply reply = { 0 };
	size_t length = ipv4_reply(packet, ICMP_MODE_ECHO, 24U);

	assert(icmp_parse(packet, length, AF_INET, ICMP_MODE_ECHO, 0x1234U, &reply));
	assert(reply.sequence == 0xabcdU);
	assert(reply.token == UINT64_C(0x0102030405060708));
	assert(reply.serial == UINT64_C(0x1112131415161718));
	assert(!icmp_parse(packet, length, AF_INET, ICMP_MODE_ECHO, 0x4321U, &reply));
	for (size_t cut = 0U; cut < length; cut++)
		assert(!icmp_parse(packet, cut, AF_INET, ICMP_MODE_ECHO, 0x1234U, &reply));
	packet[6] = 0x20U;
	assert(!icmp_parse(packet, length, AF_INET, ICMP_MODE_ECHO, 0x1234U, &reply));
	packet[6] = 0U;
	packet[24U + 9U] ^= 1U;
	assert(!icmp_parse(packet, length, AF_INET, ICMP_MODE_ECHO, 0x1234U, &reply));
	length = ipv4_reply(packet, ICMP_MODE_TIMESTAMP, 20U);
	assert(icmp_parse(packet, length, AF_INET, ICMP_MODE_TIMESTAMP, 0x1234U, &reply));
	assert(reply.originate_ms == 1000U && reply.receive_ms == 0U && reply.transmit_ms == 0U);
	assert(!icmp_parse(packet, length, AF_INET, ICMP_MODE_ECHO, 0x1234U, &reply));
	length = icmp_build(packet, AF_INET6, ICMP_MODE_ECHO, 0x1234U, 65535U, 3U, 7U, 0U);
	assert(packet[0] == 128U && packet[2] == 0U && packet[3] == 0U);
	packet[0] = 129U;
	assert(icmp_parse(packet, length, AF_INET6, ICMP_MODE_ECHO, 0x1234U, &reply));
	assert(reply.serial == 7U && reply.token == 3U && reply.sequence == 65535U);
	packet[1] = 1U;
	assert(!icmp_parse(packet, length, AF_INET6, ICMP_MODE_ECHO, 0x1234U, &reply));
	assert(!icmp_parse(packet, length, AF_INET6, ICMP_MODE_TIMESTAMP, 0x1234U, &reply));
	assert(!icmp_parse(packet, length, AF_UNIX, ICMP_MODE_ECHO, 0x1234U, &reply));
	/* Independent golden Internet checksum, including an odd final byte. */
	const unsigned char odd[] = { 0x01U, 0x02U, 0x03U };
	assert(icmp_checksum(odd, sizeof(odd)) == 0xfbfdU);
}

static void test_addresses(void)
{
	struct icmp_address a;
	struct icmp_address b;

	assert(icmp_address_parse(&a, "192.0.2.1") == 0);
	assert(icmp_address_parse(&b, "192.0.2.1") == 0);
	assert(icmp_address_equal(&a, &b));
	assert(icmp_address_parse(&b, "192.0.2.2") == 0);
	assert(!icmp_address_equal(&a, &b));
	assert(icmp_address_parse(&a, "2001:db8::1") == 0);
	assert(icmp_address_parse(&b, "2001:0db8:0:0:0:0:0:1") == 0);
	assert(icmp_address_equal(&a, &b));
	assert(icmp_address_parse(&b, "192.0.2.1") == 0);
	assert(!icmp_address_equal(&a, &b));
	assert(icmp_address_parse(&b, "example.com") == -1);
	assert(icmp_address_parse(&b, "999.1.1.1") == -1);
}

static void test_timestamps(void)
{
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(16U)];
	} buffer;
	struct msghdr message = { .msg_control = buffer.bytes, .msg_controllen = sizeof(buffer) };
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	int64_t timestamp[] = { 2147483648LL, 123456789LL };
	uint64_t result;

	memset(&buffer, 0, sizeof(buffer));
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SO_TIMESTAMPNS_NEW;
	header->cmsg_len = CMSG_LEN(sizeof(timestamp));
	memcpy(CMSG_DATA(header), timestamp, sizeof(timestamp));
	assert(icmp_timestamp(&message, &result));
	assert(result == UINT64_C(2147483648123457));
	message.msg_flags = MSG_CTRUNC;
	assert(!icmp_timestamp(&message, &result));
	message.msg_flags = 0;
	timestamp[1] = 1000000000LL;
	memcpy(CMSG_DATA(header), timestamp, sizeof(timestamp));
	assert(!icmp_timestamp(&message, &result));
	timestamp[1] = -1;
	memcpy(CMSG_DATA(header), timestamp, sizeof(timestamp));
	assert(!icmp_timestamp(&message, &result));
	timestamp[1] = 0;
	timestamp[0] = -1;
	memcpy(CMSG_DATA(header), timestamp, sizeof(timestamp));
	assert(!icmp_timestamp(&message, &result));
	message.msg_controllen = sizeof(struct cmsghdr);
	assert(!icmp_timestamp(&message, &result));
	message.msg_control = NULL;
	message.msg_controllen = 0U;
	assert(!icmp_timestamp(&message, &result));
}

/* SO_TIMESTAMPING (65): software, deprecated and hardware stamps, after a receive stamp. */
static void test_send_timestamps(void)
{
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(16U) + CMSG_SPACE(48U)];
	} buffer;
	struct msghdr message = { .msg_control = buffer.bytes, .msg_controllen = sizeof(buffer) };
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	int64_t received[] = { 1795618519LL, 907036000LL };
	int64_t stamps[6] = { 1795618519LL, 809859499LL, 0, 0, 0, 0 };
	uint64_t result;

	memset(&buffer, 0, sizeof(buffer));
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = 64;
	header->cmsg_len = CMSG_LEN(sizeof(received));
	memcpy(CMSG_DATA(header), received, sizeof(received));
	header = CMSG_NXTHDR(&message, header);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = 65;
	header->cmsg_len = CMSG_LEN(sizeof(stamps));
	memcpy(CMSG_DATA(header), stamps, sizeof(stamps));
	/* Each finds its own message: the receive stamp still parses beside the copy. */
	assert(icmp_timestamp(&message, &result) && result == UINT64_C(1795618519907036));
	assert(icmp_send_timestamp(&message, &result) && result == UINT64_C(1795618519809859));
	header->cmsg_len = CMSG_LEN(16U);
	assert(!icmp_send_timestamp(&message, &result));
	message.msg_controllen = CMSG_SPACE(16U);
	assert(!icmp_send_timestamp(&message, &result));
}

static void test_sent_monotonic(void)
{
	uint64_t sent_us = 0U;

	/* Sent 1000..1900 us monotonic; the stamp is 300 us before the realtime read after. */
	assert(icmp_sent_monotonic(1000U, 1900U, 50000U, 49700U, &sent_us) && sent_us == 1600U);
	assert(icmp_sent_monotonic(1000U, 1900U, 50000U, 50000U, &sent_us) && sent_us == 1900U);
	assert(icmp_sent_monotonic(1000U, 1900U, 50000U, 49100U, &sent_us) && sent_us == 1000U);
	/* Older than the call (an earlier send's stamp), or after it (a clock step): ignored. */
	sent_us = 7U;
	assert(!icmp_sent_monotonic(1000U, 1900U, 50000U, 49099U, &sent_us) && sent_us == 7U);
	assert(!icmp_sent_monotonic(1000U, 1900U, 50000U, 50001U, &sent_us) && sent_us == 7U);
	assert(!icmp_sent_monotonic(1900U, 1000U, 50000U, 50000U, &sent_us) && sent_us == 7U);
}

static uint64_t send_with_queue(bool stamps_enabled, const uint64_t *stamps_us, size_t count)
{
	struct icmp_socket channel = { .fd = 5,
				       .family = AF_INET,
				       .identifier = 0x1234U,
				       .send_timestamp_enabled = stamps_enabled };
	struct icmp_address address;
	char error[64];
	uint64_t sent_us = 0U;

	assert(icmp_address_parse(&address, "192.0.2.1") == 0);
	memcpy(queued_stamps_us, stamps_us, count * sizeof(stamps_us[0]));
	queued_count = count;
	queue_reads = 0U;
	monotonic_calls = 0U;
	monotonic_reads_us[0] = 1000U;
	monotonic_reads_us[1] = 1900U;
	assert(icmp_send(
		       &channel,
		       &address,
		       ICMP_MODE_ECHO,
		       1U,
		       2U,
		       3U,
		       0U,
		       &sent_us,
		       error,
		       sizeof(error)
	       ) == 0);
	return sent_us;
}

static void test_send_path(void)
{
	const uint64_t own[] = { 49700U };
	const uint64_t late_then_own[] = { 30000U, 0U, 49700U };
	const uint64_t full[16] = { 0 };
	struct icmp_socket channel = { .fd = 5, .family = AF_INET };
	const bool supported = true;
	const bool unsupported = false;

	/* The send's own stamp, 300 us before the realtime read after it, replaces the clock. */
	assert(send_with_queue(supported, own, ARRAY_SIZE(own)) == 1600U && queue_reads == 1U);
	/* An earlier send's late stamp and an entry without one are passed over. */
	assert(send_with_queue(supported, late_then_own, ARRAY_SIZE(late_then_own)) == 1600U);
	assert(queue_reads == 3U && queued_count == 0U);
	/* No stamp: the clock read before sendto() stays. */
	assert(send_with_queue(supported, own, 0U) == 1000U && queue_reads == 1U);
	/* Without kernel support the error queue is never read. */
	assert(send_with_queue(unsupported, own, ARRAY_SIZE(own)) == 1000U && queue_reads == 0U);
	/* An error event empties at most ICMP_ERROR_QUEUE_BATCH entries. */
	memcpy(queued_stamps_us, full, sizeof(full));
	queued_count = ARRAY_SIZE(full);
	queue_reads = 0U;
	icmp_discard_errors(&channel);
	assert(queue_reads == ICMP_ERROR_QUEUE_BATCH &&
	       queued_count == 16U - ICMP_ERROR_QUEUE_BATCH);
}

int main(void)
{
	test_wire();
	test_addresses();
	test_timestamps();
	test_send_timestamps();
	test_sent_monotonic();
	test_send_path();
	puts("ICMP packet tests passed");
	return 0;
}
