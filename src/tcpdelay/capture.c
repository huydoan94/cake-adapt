#define _GNU_SOURCE

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
#include <sys/ioctl.h>
#include <unistd.h>

#include "common/constants.h"
#include "common/error.h"
#include "logging/log.h"

/* Names in tcpdelay.bpf.c: the program and its maps. */
#define FILTER_PROGRAM "tcpdelay"
#define FILTER_COUNTERS "counters"
#define FILTER_ACCOUNTING "accounting"
#define FILTER_SAMPLES "samples"
#define INJECT_PROGRAM "inject_egress"
#define INJECT_SERVERS "inject_servers"
#define INJECT_HANDSHAKES "inject_handshakes"
#define INJECT_CLIENTS "inject_clients"
/* The injection maps' size while injection is off; the filter still looks them up. */
#define INJECT_UNUSED_ENTRIES 1U

static int
forward_libbpf_message(enum libbpf_print_level level, const char *format, va_list arguments)
{
	char message[ERROR_SIZE];
	size_t length;

	if (level == LIBBPF_DEBUG)
		return 0;
	(void)vsnprintf(message, sizeof(message), format, arguments);
	length = strlen(message);
	if (length > 0U && message[length - 1U] == '\n')
		message[length - 1U] = '\0';
	log_message(level == LIBBPF_WARN ? LOG_LEVEL_WARNING : LOG_LEVEL_DEBUG, "%s", message);
	return 0;
}

/* The unloaded state: nothing allocated and no socket. */
static void capture_clear(struct tcpdelay_capture *capture)
{
	bool injection = capture->injection;

	memset(capture, 0, sizeof(*capture));
	capture->injection = injection;
	capture->socket_descriptor = -1;
	capture->program_descriptor = -1;
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

static int load_program(struct tcpdelay_capture *capture, char *error, size_t error_size)
{
	struct bpf_program *program;
	struct bpf_program *injector;
	struct bpf_map *counters;
	struct bpf_map *accounting;

	capture->object = bpf_object__open_file(TCPDELAY_OBJECT_PATH, NULL);
	if (capture->object == NULL)
		return error_set(
			error,
			error_size,
			"could not open TCP delay program %s: %s",
			TCPDELAY_OBJECT_PATH,
			strerror(errno)
		);
	counters = bpf_object__find_map_by_name(capture->object, FILTER_COUNTERS);
	accounting = bpf_object__find_map_by_name(capture->object, FILTER_ACCOUNTING);
	injector = bpf_object__find_program_by_name(capture->object, INJECT_PROGRAM);
	if (injector == NULL || bpf_program__set_autoload(injector, capture->injection) != 0 ||
	    (!capture->injection &&
	     (bpf_map__set_max_entries(
		      bpf_object__find_map_by_name(capture->object, INJECT_SERVERS),
		      INJECT_UNUSED_ENTRIES
	      ) != 0 ||
	      bpf_map__set_max_entries(
		      bpf_object__find_map_by_name(capture->object, INJECT_HANDSHAKES),
		      INJECT_UNUSED_ENTRIES
	      ) != 0 ||
	      bpf_map__set_max_entries(
		      bpf_object__find_map_by_name(capture->object, INJECT_CLIENTS),
		      INJECT_UNUSED_ENTRIES
	      ) != 0)))
		return error_set(error, error_size, "TCP delay object lacks the timestamp injector");
	if (counters == NULL || accounting == NULL ||
	    bpf_map__value_size(counters) != sizeof(struct tcpdelay_counters) ||
	    bpf_map__value_size(accounting) != sizeof(struct tcpdelay_accounting) ||
	    bpf_map__key_size(counters) != sizeof(uint32_t) ||
	    bpf_map__key_size(accounting) != sizeof(uint32_t) ||
	    bpf_map__type(counters) != BPF_MAP_TYPE_PERCPU_ARRAY ||
	    bpf_map__type(accounting) != BPF_MAP_TYPE_ARRAY ||
	    bpf_map__max_entries(counters) != 1U || bpf_map__max_entries(accounting) != 1U)
		return error_set(
			error,
			error_size,
			"TCP delay object has incompatible accounting maps"
		);
	if (bpf_object__load(capture->object) != 0)
		return error_set(
			error,
			error_size,
			"could not load TCP delay program %s: %s",
			TCPDELAY_OBJECT_PATH,
			strerror(errno)
		);
	program = bpf_object__find_program_by_name(capture->object, FILTER_PROGRAM);
	capture->counters_descriptor = bpf_map__fd(counters);
	capture->accounting_descriptor = bpf_map__fd(accounting);
	capture->ring = ring_buffer__new(
		bpf_object__find_map_fd_by_name(capture->object, FILTER_SAMPLES),
		add_record,
		&capture->estimator,
		NULL
	);
	if (program == NULL || capture->counters_descriptor < 0 || capture->ring == NULL)
		return error_set(
			error,
			error_size,
			"TCP delay program %s lacks its program or maps",
			TCPDELAY_OBJECT_PATH
		);
	capture->program_descriptor = bpf_program__fd(program);
	return 0;
}

/*
 * Protocol 0 receives nothing until bind(), so no packet is queued before the
 * filter is attached. The filter accepts no packet, so the socket's receive
 * queue stays empty.
 */
static int attach_socket(
	struct tcpdelay_capture *capture,
	const char *interface,
	char *error,
	size_t error_size
)
{
	struct sockaddr_ll address = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
		.sll_ifindex = (int)capture->interface_index,
	};
	struct ifreq request = { 0 };
	uint32_t key = 0U;

	capture->socket_descriptor = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
	if (capture->socket_descriptor < 0)
		goto fail;
	if (capture->accounting.enabled) {
		(void)snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
		if (ioctl(capture->socket_descriptor, SIOCGIFHWADDR, &request) != 0)
			goto fail;
		capture->accounting.hardware_type =
			(uint32_t)(unsigned short)request.ifr_hwaddr.sa_family;
	}
	if (bpf_map_update_elem(capture->accounting_descriptor, &key, &capture->accounting, BPF_ANY) !=
		    0 ||
	    setsockopt(
		    capture->socket_descriptor,
		    SOL_SOCKET,
		    SO_ATTACH_BPF,
		    &capture->program_descriptor,
		    sizeof(capture->program_descriptor)
	    ) != 0 ||
	    bind(capture->socket_descriptor, (struct sockaddr *)&address, sizeof(address)) != 0)
		goto fail;
	return 0;

fail:
	return error_set(
		error,
		error_size,
		"could not attach TCP delay program to %s: %s",
		interface,
		strerror(errno)
	);
}

void tcpdelay_capture_init(struct tcpdelay_capture *capture)
{
	memset(capture, 0, sizeof(*capture));
	capture_clear(capture);
}

void tcpdelay_capture_enable_injection(struct tcpdelay_capture *capture)
{
	capture->injection = true;
}

int tcpdelay_capture_load(struct tcpdelay_capture *capture, char *error, size_t error_size)
{
	int cpus;

	if (capture->object != NULL)
		return 0;
	libbpf_set_print(forward_libbpf_message);
	cpus = libbpf_num_possible_cpus();
	if (cpus <= 0)
		return error_set(
			error,
			error_size,
			"could not count CPUs for TCP delay counters: %s",
			strerror(-cpus)
		);
	capture->cpu_count = (size_t)cpus;
	capture->counter_values = calloc(capture->cpu_count, sizeof(*capture->counter_values));
	if (capture->counter_values == NULL) {
		error_set(
			error,
			error_size,
			"could not allocate TCP delay counters: %s",
			strerror(errno)
		);
		goto fail;
	}
	if (load_program(capture, error, error_size) != 0)
		goto fail;
	return 0;

fail:
	tcpdelay_capture_unload(capture);
	return -1;
}

/* Zeroes every CPU's counters, so a new capture starts from zero. */
static int reset_counters(struct tcpdelay_capture *capture)
{
	uint32_t key = 0U;

	memset(capture->counter_values, 0, capture->cpu_count * sizeof(*capture->counter_values));
	return bpf_map_update_elem(
		capture->counters_descriptor,
		&key,
		capture->counter_values,
		BPF_ANY
	);
}

int tcpdelay_capture_open(
	struct tcpdelay_capture *capture,
	const char *interface,
	const struct cake_accounting *accounting,
	char *error,
	size_t error_size
)
{
	if (tcpdelay_capture_load(capture, error, error_size) != 0)
		return -1;
	tcpdelay_capture_close(capture);
	memset(&capture->accounting, 0, sizeof(capture->accounting));
	if (accounting != NULL) {
		capture->accounting.cake = *accounting;
		capture->accounting.enabled = 1U;
	}
	capture->interface_index = if_nametoindex(interface);
	if (capture->interface_index == 0U)
		return error_set(
			error,
			error_size,
			"TCP delay interface %s: %s",
			interface,
			strerror(errno)
		);
	/* Records left from the previous socket are dropped with the estimator's state. */
	(void)ring_buffer__consume(capture->ring);
	memset(&capture->estimator, 0, sizeof(capture->estimator));
	if (reset_counters(capture) != 0) {
		return error_set(
			error,
			error_size,
			"could not reset TCP delay counters: %s",
			strerror(errno)
		);
	}
	if (attach_socket(capture, interface, error, error_size) != 0) {
		tcpdelay_capture_close(capture);
		return -1;
	}
	return 0;
}

int tcpdelay_capture_drain(struct tcpdelay_capture *capture)
{
	return ring_buffer__consume(capture->ring);
}

int tcpdelay_capture_counters(
	const struct tcpdelay_capture *capture,
	struct tcpdelay_counters *counters
)
{
	uint32_t key = 0U;
	size_t cpu;

	memset(counters, 0, sizeof(*counters));
	if (bpf_map_lookup_elem(capture->counters_descriptor, &key, capture->counter_values) != 0)
		return -1;
	for (cpu = 0U; cpu < capture->cpu_count; cpu++) {
		const struct tcpdelay_counters *value = &capture->counter_values[cpu];

		counters->ring_full += value->ring_full;
		counters->ack_bytes += value->ack_bytes;
		counters->upload_bytes += value->upload_bytes;
		counters->unaccounted_packets += value->unaccounted_packets;
	}
	return 0;
}

void tcpdelay_capture_close(struct tcpdelay_capture *capture)
{
	if (capture->socket_descriptor >= 0)
		(void)close(capture->socket_descriptor);
	capture->socket_descriptor = -1;
	capture->interface_index = 0U;
}

void tcpdelay_capture_unload(struct tcpdelay_capture *capture)
{
	tcpdelay_capture_close(capture);
	ring_buffer__free(capture->ring);
	bpf_object__close(capture->object);
	free(capture->counter_values);
	capture_clear(capture);
}
