// SPDX-License-Identifier: GPL-2.0-only
/*
 * tcpthink: request-response traffic on one persistent TCP connection, with
 * a server that thinks before it answers, like an API or game server.
 *
 *   tcpthink -s PORT THINK_MS[-MAX_MS] RESPONSE_BYTES
 *       serve every connection: read a request, wait THINK_MS (or a random
 *       time from THINK_MS to MAX_MS), reply with RESPONSE_BYTES
 *   tcpthink -c ADDRESS PORT SECONDS GAP_MS REQUEST_BYTES RESPONSE_BYTES
 *       send a request, read the whole reply, wait GAP_MS, repeat for SECONDS;
 *       prints the requests made and their mean and largest response time.
 *       A connection that hangs is killed 5 s after SECONDS (SIGALRM).
 *
 * ADDRESS is IPv4 or IPv6; the server listens on both.
 *
 * The reply to a request echoes the request's TCP timestamp after the think
 * time, so a passive estimator that reads echo delay as upload queue sees the
 * think time unless it can tell the server was busy. A constant think time
 * becomes part of the flow's floor; a varying one does not.
 */
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_BYTES 65536

static unsigned char buffer[BUFFER_BYTES];

static double now_s(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void sleep_ms(long ms)
{
	struct timespec wait = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&wait, &wait) != 0)
		;
}

static int write_all(int fd, size_t bytes)
{
	while (bytes > 0) {
		size_t chunk = bytes < BUFFER_BYTES ? bytes : BUFFER_BYTES;
		ssize_t written = write(fd, buffer, chunk);

		if (written <= 0)
			return -1;
		bytes -= (size_t)written;
	}
	return 0;
}

static int read_all(int fd, size_t bytes)
{
	while (bytes > 0) {
		size_t chunk = bytes < BUFFER_BYTES ? bytes : BUFFER_BYTES;
		ssize_t got = read(fd, buffer, chunk);

		if (got <= 0)
			return -1;
		bytes -= (size_t)got;
	}
	return 0;
}

static void serve(int fd, long think_ms, long think_max_ms, size_t response_bytes)
{
	srandom((unsigned int)getpid());
	/* A request is whatever one read returns; requests are small. */
	while (read(fd, buffer, BUFFER_BYTES) > 0) {
		sleep_ms(think_ms + (think_max_ms > think_ms ? random() % (think_max_ms - think_ms + 1) : 0));
		if (write_all(fd, response_bytes) != 0)
			break;
	}
	close(fd);
}

static int server(int port, long think_ms, long think_max_ms, size_t response_bytes)
{
	/* :: with IPV6_V6ONLY off takes IPv4 too, as ::ffff:a.b.c.d. */
	struct sockaddr_in6 address = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port) };
	int one = 1;
	int zero = 0;
	int listener = socket(AF_INET6, SOCK_STREAM, 0);

	signal(SIGCHLD, SIG_IGN);
	if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
	    setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)) != 0 ||
	    bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 16) != 0) {
		perror("tcpthink server");
		return 1;
	}
	for (;;) {
		int fd = accept(listener, NULL, NULL);

		if (fd < 0)
			continue;
		if (fork() == 0) {
			close(listener);
			serve(fd, think_ms, think_max_ms, response_bytes);
			_exit(0);
		}
		close(fd);
	}
}

static int client(
	const char *host,
	int port,
	double seconds,
	long gap_ms,
	size_t request_bytes,
	size_t response_bytes
)
{
	struct sockaddr_storage address = { 0 };
	struct sockaddr_in *v4 = (struct sockaddr_in *)&address;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address;
	socklen_t length;
	int fd;
	int one = 1;
	double end, total = 0, largest = 0;
	long requests = 0;

	/* A hung connection must not hang the test: the alarm ends the process. */
	alarm((unsigned int)seconds + 5U);
	if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
		v4->sin_family = AF_INET;
		v4->sin_port = htons((uint16_t)port);
		length = sizeof(*v4);
	} else if (inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
		v6->sin6_family = AF_INET6;
		v6->sin6_port = htons((uint16_t)port);
		length = sizeof(*v6);
	} else {
		fprintf(stderr, "tcpthink client: bad address %s\n", host);
		return 1;
	}
	fd = socket(address.ss_family, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&address, length) != 0) {
		perror("tcpthink client");
		return 1;
	}
	/* Requests leave at once, as small RPCs do. */
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	end = now_s() + seconds;
	while (now_s() < end) {
		double start = now_s(), took;

		if (write_all(fd, request_bytes) != 0 || read_all(fd, response_bytes) != 0) {
			perror("tcpthink client");
			return 1;
		}
		took = now_s() - start;
		total += took;
		if (took > largest)
			largest = took;
		requests++;
		sleep_ms(gap_ms);
	}
	close(fd);
	printf("requests %ld mean_ms %.1f max_ms %.1f\n", requests,
	       requests ? 1000 * total / (double)requests : 0.0, 1000 * largest);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 5 && strcmp(argv[1], "-s") == 0) {
		const char *dash = strchr(argv[3], '-');

		return server(atoi(argv[2]), atol(argv[3]), dash ? atol(dash + 1) : atol(argv[3]),
			      (size_t)atol(argv[4]));
	}
	if (argc == 8 && strcmp(argv[1], "-c") == 0)
		return client(argv[2], atoi(argv[3]), atof(argv[4]), atol(argv[5]),
			      (size_t)atol(argv[6]), (size_t)atol(argv[7]));
	fprintf(stderr, "usage: tcpthink -s PORT THINK_MS[-MAX_MS] RESPONSE_BYTES\n"
			"       tcpthink -c ADDRESS PORT SECONDS GAP_MS REQUEST_BYTES RESPONSE_BYTES\n");
	return 2;
}
