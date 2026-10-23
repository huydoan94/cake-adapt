#define _GNU_SOURCE

#include "latency/icmp.h"
#include "constants/icmp.h"

#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
	test_wire();
	test_addresses();
	test_timestamps();
	puts("ICMP packet tests passed");
	return 0;
}
