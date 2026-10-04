/*
 * udpping: constant-rate UDP echo for VoIP-like latency and loss.
 *
 *   udpping -s PORT                          echo server
 *   udpping HOST PORT SIZE INTERVAL_MS SECONDS
 *
 * The client sends SIZE-byte datagrams every INTERVAL_MS and prints
 * "seq rtt_us" per echoed datagram, then "# sent N received M".
 *
 * Build with the OpenWrt toolchain for the VM, for example:
 *   "$SDK"/staging_dir/toolchain-<target>/bin/<target>-gcc -O2 -Wall -Wextra -o udpping udpping.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct header {
	uint32_t sequence;
	uint64_t sent_ns;
};

static uint64_t now_ns(void)
{
	struct timespec value;

	clock_gettime(CLOCK_MONOTONIC, &value);
	return (uint64_t)value.tv_sec * 1000000000U + (uint64_t)value.tv_nsec;
}

static int serve(int port)
{
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	char buffer[2048];
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		perror("bind");
		return 1;
	}
	for (;;) {
		struct sockaddr_in peer;
		socklen_t length = sizeof(peer);
		ssize_t size =
			recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&peer, &length);

		if (size > 0)
			(void)sendto(fd, buffer, (size_t)size, 0, (struct sockaddr *)&peer, length);
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_in address = { .sin_family = AF_INET };
	char buffer[2048] = { 0 };
	size_t size;
	uint64_t interval_ns;
	uint64_t end_ns;
	uint64_t next_ns;
	uint32_t sent = 0;
	uint32_t received = 0;
	int fd;

	if (argc == 3 && strcmp(argv[1], "-s") == 0)
		return serve(atoi(argv[2]));
	if (argc != 6) {
		fprintf(stderr,
			"usage: udpping -s PORT | udpping HOST PORT SIZE INTERVAL_MS SECONDS\n");
		return 2;
	}
	inet_pton(AF_INET, argv[1], &address.sin_addr);
	address.sin_port = htons((uint16_t)atoi(argv[2]));
	size = (size_t)atoi(argv[3]);
	if (size < sizeof(struct header) || size > sizeof(buffer))
		return 2;
	interval_ns = (uint64_t)atoi(argv[4]) * 1000000U;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	/* Unconnected: a server with several addresses may answer from another one. */
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	next_ns = now_ns();
	/* Wait one extra second for late echoes. */
	end_ns = next_ns + (uint64_t)atoi(argv[5]) * 1000000000U;
	while (now_ns() < end_ns + 1000000000U) {
		uint64_t now = now_ns();
		struct pollfd poller = { .fd = fd, .events = POLLIN };
		int timeout_ms;

		if (now >= next_ns && now < end_ns) {
			struct header header = { .sequence = sent++, .sent_ns = now };

			memcpy(buffer, &header, sizeof(header));
			(
				void
			)sendto(fd, buffer, size, 0, (struct sockaddr *)&address, sizeof(address));
			next_ns += interval_ns;
			continue;
		}
		timeout_ms = now < next_ns && now < end_ns ? (int)((next_ns - now) / 1000000U) + 1 :
							     50;
		if (poll(&poller, 1, timeout_ms) > 0) {
			struct header header;

			if (recv(fd, buffer, sizeof(buffer), 0) >= (ssize_t)sizeof(header)) {
				memcpy(&header, buffer, sizeof(header));
				printf("%u %llu\n",
				       header.sequence,
				       (unsigned long long)((now_ns() - header.sent_ns) / 1000U));
				received++;
			}
		}
	}
	printf("# sent %u received %u\n", sent, received);
	return 0;
}
