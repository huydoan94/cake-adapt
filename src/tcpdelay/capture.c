#include "tcpdelay/capture.h"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/error.h"
#include "logging/log.h"

#define FILTER_PROGRAM "tcpdelay"

static int forward_libbpf_message(enum libbpf_print_level level, const char *format,
				  va_list arguments)
{
	char message[ERROR_SIZE];
	size_t length;

	if (level == LIBBPF_DEBUG)
		return 0;
	vsnprintf(message, sizeof(message), format, arguments);
	length = strlen(message);
	if (length > 0U && message[length - 1U] == '\n')
		message[length - 1U] = '\0';
	log_message(level == LIBBPF_WARN ? LOG_LEVEL_WARNING : LOG_LEVEL_DEBUG, "%s", message);
	return 0;
}

static int add_record(void *context, void *data, size_t size)
{
	struct tcpdelay_estimator *estimator = context;
	const struct tcpdelay_record *record = data;
	struct tcpdelay_sample sample;

	if (size < sizeof(*record))
		return 0;
	sample.flow = record->flow;
	sample.arrival_ns = record->arrival_ns;
	sample.departure_ns = record->departure_ns;
	sample.tsval = record->tsval;
	tcpdelay_estimator_add(estimator, &sample);
	return 0;
}

int tcpdelay_capture_open(struct tcpdelay_capture *capture, const char *object_path,
			  const char *interface, struct tcpdelay_estimator *estimator, char *error,
			  size_t error_size)
{
	struct sockaddr_ll address = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL) };
	struct bpf_program *program;
	int program_descriptor;
	int cpus;

	memset(capture, 0, sizeof(*capture));
	capture->socket_descriptor = -1;
	capture->estimator = estimator;
	libbpf_set_print(forward_libbpf_message);

	capture->interface_index = if_nametoindex(interface);
	address.sll_ifindex = (int)capture->interface_index;
	if (address.sll_ifindex == 0) {
		error_set(error, error_size, "TCP delay interface %s: %s", interface,
			  strerror(errno));
		return -1;
	}
	cpus = libbpf_num_possible_cpus();
	if (cpus <= 0) {
		error_set(error, error_size, "could not count CPUs for TCP delay counters: %s",
			  strerror(-cpus));
		return -1;
	}
	capture->cpu_count = (size_t)cpus;
	capture->counter_values = calloc(capture->cpu_count, sizeof(*capture->counter_values));
	capture->object = bpf_object__open_file(object_path, NULL);
	if (capture->counter_values == NULL || capture->object == NULL) {
		error_set(error, error_size, "could not open TCP delay program %s: %s", object_path,
			  strerror(errno));
		goto fail;
	}
	if (bpf_object__load(capture->object) != 0) {
		error_set(error, error_size, "could not load TCP delay program %s: %s", object_path,
			  strerror(errno));
		goto fail;
	}
	program = bpf_object__find_program_by_name(capture->object, FILTER_PROGRAM);
	capture->counters_descriptor = bpf_object__find_map_fd_by_name(capture->object, "counters");
	capture->ring =
		ring_buffer__new(bpf_object__find_map_fd_by_name(capture->object, "samples"),
				 add_record, estimator, NULL);
	if (program == NULL || capture->counters_descriptor < 0 || capture->ring == NULL) {
		error_set(error, error_size, "TCP delay program %s lacks its program or maps",
			  object_path);
		goto fail;
	}
	program_descriptor = bpf_program__fd(program);

	/*
     * Protocol 0 receives nothing until bind(), so no packet is queued before
     * the filter is attached. The filter accepts no packet, so the socket's
     * receive queue stays empty.
     */
	capture->socket_descriptor = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (capture->socket_descriptor < 0 ||
	    setsockopt(capture->socket_descriptor, SOL_SOCKET, SO_ATTACH_BPF, &program_descriptor,
		       sizeof(program_descriptor)) != 0 ||
	    bind(capture->socket_descriptor, (struct sockaddr *)&address, sizeof(address)) != 0) {
		error_set(error, error_size, "could not attach TCP delay program to %s: %s",
			  interface, strerror(errno));
		goto fail;
	}
	return 0;

fail:
	tcpdelay_capture_close(capture);
	return -1;
}

int tcpdelay_capture_drain(struct tcpdelay_capture *capture)
{
	return ring_buffer__consume(capture->ring);
}

int tcpdelay_capture_counters(const struct tcpdelay_capture *capture,
			      struct tcpdelay_counters *counters)
{
	uint32_t key = 0U;
	size_t cpu;

	memset(counters, 0, sizeof(*counters));
	if (bpf_map_lookup_elem(capture->counters_descriptor, &key, capture->counter_values) != 0)
		return -1;
	for (cpu = 0U; cpu < capture->cpu_count; cpu++) {
		counters->ring_full += capture->counter_values[cpu].ring_full;
		counters->ack_bytes += capture->counter_values[cpu].ack_bytes;
		counters->upload_bytes += capture->counter_values[cpu].upload_bytes;
	}
	return 0;
}

void tcpdelay_capture_close(struct tcpdelay_capture *capture)
{
	if (capture->socket_descriptor >= 0)
		close(capture->socket_descriptor);
	ring_buffer__free(capture->ring);
	bpf_object__close(capture->object);
	free(capture->counter_values);
	memset(capture, 0, sizeof(*capture));
	capture->socket_descriptor = -1;
}
