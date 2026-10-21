#define _GNU_SOURCE

/* Bounded probe driver for the VM spike, not installed with the daemon. */
#include "latency/native.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	struct native_session session;
	const char *targets[1];
	char error[256] = "";
	uint64_t count;
	uint64_t interval_us;
	uint64_t start_us;
	uint64_t deadline_us;
	uint64_t received = 0U;
	int outcome = 1;

	if (argc != 6 || !parse_unsigned(argv[4], argv[4] + strlen(argv[4]), &count) ||
	    !parse_unsigned(argv[5], argv[5] + strlen(argv[5]), &interval_us) || count == 0U ||
	    count > 1000U || interval_us < MILLISECOND || interval_us > SECOND ||
	    !native_method(argv[2])) {
		fprintf(stderr,
			"usage: icmp_driver INTERFACE native|native-ts TARGET COUNT PERIOD_US\n");
		return 2;
	}
	native_init(&session);
	if (!read_clock_us(CLOCK_MONOTONIC, &start_us))
		return 1;
	targets[0] = argv[3];
	if (native_open(
		    &session,
		    argv[2],
		    argv[1],
		    targets,
		    1U,
		    interval_us,
		    start_us,
		    error,
		    sizeof(error)
	    ) != 0) {
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	deadline_us = start_us + count * interval_us + 10U * SECOND;
	printf("mode=%s interface=%s target=%s count=%" PRIu64 " period_us=%" PRIu64 "\n",
	       argv[2],
	       argv[1],
	       argv[3],
	       count,
	       interval_us);
	puts(
		"serial,receive_realtime_us,rtt_us,upload_owd_us,download_owd_us,kernel_timestamp,originate_ms,receive_ms,transmit_ms,sent_monotonic_us,read_monotonic_us,read_realtime_us"
	);
	while (received < count) {
		struct pollfd descriptors[2];
		uint64_t now_us;
		uint64_t realtime_us;
		uint64_t delay_us;
		int ready;

		if (!read_clock_us(CLOCK_MONOTONIC, &now_us) ||
		    !read_clock_us(CLOCK_REALTIME, &realtime_us) || now_us >= deadline_us)
			break;
		if (session.next_serial < count &&
		    native_tick(&session, now_us, realtime_us, error, sizeof(error)) != 0)
			break;
		for (size_t index = 0U; index < ARRAY_SIZE(descriptors); index++)
			descriptors[index] = (struct pollfd){ .fd = session.channels[index].fd,
							      .events = POLLIN };
		if (!read_clock_us(CLOCK_MONOTONIC, &now_us))
			break;
		delay_us = session.next_serial < count ?
				   saturating_sub(native_next_event(&session), now_us) :
				   100U * MILLISECOND;
		ready = poll(
			descriptors,
			ARRAY_SIZE(descriptors),
			(int)min_u64(delay_us / MILLISECOND + (delay_us % MILLISECOND != 0U), 100U)
		);
		if (ready < 0 && errno != EINTR)
			break;
		for (size_t index = 0U; index < ARRAY_SIZE(descriptors); index++) {
			if ((descriptors[index].revents & POLLIN) == 0)
				continue;
			/* The daemon reads 64 replies per wake-up; the driver does the same. */
			for (size_t budget = 0U; budget < 64U; budget++) {
				struct icmp_reply reply;
				struct latency_sample sample;
				uint64_t response_us;
				enum icmp_receive_result result = icmp_receive(
					&session.channels[index],
					session.mode,
					&reply,
					error,
					sizeof(error)
				);

				if (result == ICMP_RECEIVE_NONE)
					break;
				if (result == ICMP_RECEIVE_ERROR)
					goto done;
				if (result == ICMP_RECEIVE_IGNORED)
					continue;
				if (!read_clock_us(CLOCK_MONOTONIC, &now_us) ||
				    !read_clock_us(CLOCK_REALTIME, &realtime_us))
					goto done;
				if (!native_accept(
					    &session,
					    &reply,
					    now_us,
					    realtime_us,
					    &sample,
					    &response_us
				    ))
					continue;
				printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRId64
				       ",%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu64
				       ",%" PRIu64 ",%" PRIu64 "\n",
				       sample.sequence,
				       reply.realtime_us,
				       response_us -
					       session.requests[reply.sequence % session.capacity]
						       .sent_us,
				       sample.upload_owd_us,
				       sample.download_owd_us,
				       reply.kernel_timestamp ? 1U : 0U,
				       reply.originate_ms,
				       reply.receive_ms,
				       reply.transmit_ms,
				       session.requests[reply.sequence % session.capacity].sent_us,
				       now_us,
				       realtime_us);
				received++;
			}
		}
	}
	outcome = received == count ? 0 : 1;
done:
	fprintf(stderr, "received=%" PRIu64 "/%" PRIu64 " error=%s\n", received, count, error);
	native_close(&session);
	return outcome;
}
