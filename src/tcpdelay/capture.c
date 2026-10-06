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
#include <time.h>
#include <unistd.h>

#include "common/error.h"
#include "common/helpers.h"
#include "common/constants.h"
#include "logging/log.h"

/* Names in tcpdelay.bpf.c: the program and its maps. */
#define FILTER_PROGRAM "tcpdelay"
#define FILTER_COUNTERS "counters"
#define FILTER_ACCOUNTING "accounting"
#define FILTER_SAMPLES "samples"
#define FILTER_STREAM_EPOCH "stream_epoch_v1"
#define FILTER_STREAM_LOSSES "stream_losses"
#define FILTER_STREAM_FAULT "stream_fault"
#define STREAM_MAP_KEY_SIZE sizeof(uint32_t)
#define STREAM_MAP_VALUE_SIZE sizeof(uint32_t)
#define PERCPU_VALUE_STRIDE 8U
#define TCPDELAY_RECOVERY_COOLDOWN_MICROSECONDS (1U * SECOND)

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

/* The closed state: nothing allocated and no socket. */
static void capture_clear(struct tcpdelay_capture *capture)
{
	memset(capture, 0, sizeof(*capture));
	capture->socket_descriptor = -1;
	capture->epoch_descriptor = -1;
	capture->loss_descriptor = -1;
	capture->fault_descriptor = -1;
}

static int add_record(void *context, void *data, size_t size)
{
	struct tcpdelay_capture *capture = context;
	const struct tcpdelay_record *record = data;
	uint32_t epoch;
	uint32_t version;
	uint64_t departure_ns;

	if (size != sizeof(*record)) {
		capture->uncertain_pending = true;
		return 0;
	}
	epoch = (record->metadata & TCPDELAY_STREAM_EPOCH_MASK) >> TCPDELAY_STREAM_EPOCH_SHIFT;
	version = (record->metadata & TCPDELAY_STREAM_VERSION_MASK) >>
		  TCPDELAY_STREAM_VERSION_SHIFT;
	if (epoch < capture->epoch)
		return 0;
	if (capture->state != TCPDELAY_CAPTURE_READY)
		return 0;
	if (epoch != capture->epoch || version != TCPDELAY_STREAM_VERSION) {
		capture->uncertain_pending = true;
		return 0;
	}
	if (!tcpdelay_record_valid(record, capture->epoch)) {
		capture->uncertain_pending = true;
		return 0;
	}
	if (!tcpdelay_lifetime_add(capture->lifetime, capture->estimator, record, &departure_ns))
		capture->uncertain_pending = true;
	return 0;
}

static bool compatible_stream_map(
	const struct bpf_map *map,
	uint32_t type,
	uint32_t value_size,
	uint32_t max_entries
)
{
	return map != NULL && bpf_map__type(map) == type &&
	       bpf_map__key_size(map) == STREAM_MAP_KEY_SIZE &&
	       bpf_map__value_size(map) == value_size && bpf_map__max_entries(map) == max_entries;
}

/* Returns the filter's program descriptor, or -1. */
static int load_program(
	struct tcpdelay_capture *capture,
	const char *object_path,
	char *error,
	size_t error_size
)
{
	struct bpf_program *program;
	struct bpf_map *counters;
	struct bpf_map *accounting;
	struct bpf_map *epoch;
	struct bpf_map *losses;
	struct bpf_map *fault;
	struct bpf_map *samples;
	uint32_t key = 0U;
	uint32_t initial_epoch = 1U;

	capture->object = bpf_object__open_file(object_path, NULL);
	if (capture->object == NULL)
		return error_set(
			error,
			error_size,
			"could not open TCP delay program %s: %s",
			object_path,
			strerror(errno)
		);
	counters = bpf_object__find_map_by_name(capture->object, FILTER_COUNTERS);
	accounting = bpf_object__find_map_by_name(capture->object, FILTER_ACCOUNTING);
	epoch = bpf_object__find_map_by_name(capture->object, FILTER_STREAM_EPOCH);
	losses = bpf_object__find_map_by_name(capture->object, FILTER_STREAM_LOSSES);
	fault = bpf_object__find_map_by_name(capture->object, FILTER_STREAM_FAULT);
	samples = bpf_object__find_map_by_name(capture->object, FILTER_SAMPLES);
	if (counters == NULL || accounting == NULL ||
	    bpf_map__value_size(counters) != sizeof(struct tcpdelay_counters) ||
	    bpf_map__value_size(accounting) != sizeof(struct tcpdelay_accounting) ||
	    bpf_map__key_size(counters) != sizeof(uint32_t) ||
	    bpf_map__key_size(accounting) != sizeof(uint32_t) ||
	    bpf_map__type(counters) != BPF_MAP_TYPE_PERCPU_ARRAY ||
	    bpf_map__type(accounting) != BPF_MAP_TYPE_ARRAY ||
	    bpf_map__max_entries(counters) != 1U || bpf_map__max_entries(accounting) != 1U ||
	    !compatible_stream_map(epoch, BPF_MAP_TYPE_HASH, STREAM_MAP_VALUE_SIZE, 1U) ||
	    !compatible_stream_map(
		    losses,
		    BPF_MAP_TYPE_HASH,
		    STREAM_MAP_VALUE_SIZE,
		    TCPDELAY_STREAM_LOSSES
	    ) ||
	    !compatible_stream_map(fault, BPF_MAP_TYPE_PERCPU_ARRAY, STREAM_MAP_VALUE_SIZE, 1U))
		return error_set(error, error_size, "TCP delay object has incompatible maps");
	if (samples == NULL || bpf_map__type(samples) != BPF_MAP_TYPE_RINGBUF ||
	    bpf_map__max_entries(samples) != TCPDELAY_RING_BYTES)
		return error_set(
			error,
			error_size,
			"TCP delay object has an incompatible sample ring"
		);
	if (bpf_object__load(capture->object) != 0)
		return error_set(
			error,
			error_size,
			"could not load TCP delay program %s: %s",
			object_path,
			strerror(errno)
		);
	capture->epoch_descriptor = bpf_map__fd(epoch);
	capture->loss_descriptor = bpf_map__fd(losses);
	capture->fault_descriptor = bpf_map__fd(fault);
	if (capture->epoch_descriptor < 0 || capture->loss_descriptor < 0 ||
	    capture->fault_descriptor < 0 ||
	    bpf_map_update_elem(capture->epoch_descriptor, &key, &initial_epoch, BPF_ANY) != 0)
		return error_set(
			error,
			error_size,
			"TCP delay object lacks usable lifetime stream maps"
		);
	capture->epoch = initial_epoch;
	capture->state = TCPDELAY_CAPTURE_READY;
	program = bpf_object__find_program_by_name(capture->object, FILTER_PROGRAM);
	capture->counters_descriptor = bpf_map__fd(counters);
	capture->accounting_descriptor = bpf_map__fd(accounting);
	capture->ring = ring_buffer__new(
		bpf_object__find_map_fd_by_name(capture->object, FILTER_SAMPLES),
		add_record,
		capture,
		NULL
	);
	if (program == NULL || capture->counters_descriptor < 0 || capture->ring == NULL)
		return error_set(
			error,
			error_size,
			"TCP delay program %s lacks its program or maps",
			object_path
		);
	return bpf_program__fd(program);
}

static void reset_timing(struct tcpdelay_capture *capture, uint32_t epoch)
{
	enum tcpdelay_baseline_policy policy = capture->estimator->baseline_policy;

	tcpdelay_estimator_init(capture->estimator);
	tcpdelay_estimator_set_policy(capture->estimator, policy);
	tcpdelay_lifetime_reset(capture->lifetime, epoch);
}

static void begin_recovery(struct tcpdelay_capture *capture, uint64_t now_microseconds)
{
	if (capture->state != TCPDELAY_CAPTURE_READY)
		return;
	reset_timing(capture, capture->epoch);
	capture->state = TCPDELAY_CAPTURE_RECOVERING;
	capture->retry_after_microseconds =
		now_microseconds + TCPDELAY_RECOVERY_COOLDOWN_MICROSECONDS;
}

static int stream_faulted(struct tcpdelay_capture *capture)
{
	uint32_t key = 0U;
	size_t cpu;

	memset(capture->fault_values, 0, capture->cpu_count * PERCPU_VALUE_STRIDE);
	if (bpf_map_lookup_elem(capture->fault_descriptor, &key, capture->fault_values) != 0)
		return -1;
	for (cpu = 0U; cpu < capture->cpu_count; cpu++) {
		uint32_t value;

		memcpy(&value, capture->fault_values + cpu * PERCPU_VALUE_STRIDE, sizeof(value));
		if (value != 0U)
			return 1;
	}
	return 0;
}

/* 1 means loss marker present, 0 absent, and -1 is a map-read failure. */
static int stream_lost_for_epoch(const struct tcpdelay_capture *capture)
{
	uint32_t marker = 0U;

	if (bpf_map_lookup_elem(capture->loss_descriptor, &capture->epoch, &marker) == 0)
		return marker == 1U ? 1 : -1;
	return errno == ENOENT ? 0 : -1;
}

static bool inspect_stream(struct tcpdelay_capture *capture)
{
	uint32_t key = 0U;
	uint32_t epoch = 0U;
	int faulted;
	int lost;

	if (bpf_map_lookup_elem(capture->epoch_descriptor, &key, &epoch) != 0 ||
	    epoch != capture->epoch) {
		capture->state = TCPDELAY_CAPTURE_DISABLED;
		return false;
	}
	faulted = stream_faulted(capture);
	lost = stream_lost_for_epoch(capture);
	if (faulted != 0 || lost < 0) {
		capture->state = TCPDELAY_CAPTURE_DISABLED;
		return false;
	}
	if (lost != 0)
		capture->uncertain_pending = true;
	return true;
}

static void recover_stream(struct tcpdelay_capture *capture)
{
	uint64_t now_microseconds;
	uint32_t next_epoch;
	uint32_t key = 0U;

	if (!read_clock_microseconds(CLOCK_MONOTONIC, &now_microseconds)) {
		capture->state = TCPDELAY_CAPTURE_DISABLED;
		return;
	}
	if (capture->uncertain_pending) {
		capture->uncertain_pending = false;
		begin_recovery(capture, now_microseconds);
	}
	if (capture->state != TCPDELAY_CAPTURE_RECOVERING ||
	    now_microseconds < capture->retry_after_microseconds)
		return;
	if (capture->epoch >= TCPDELAY_STREAM_EPOCHS) {
		capture->state = TCPDELAY_CAPTURE_DISABLED;
		return;
	}
	next_epoch = capture->epoch + 1U;
	if (bpf_map_update_elem(capture->epoch_descriptor, &key, &next_epoch, BPF_ANY) != 0) {
		capture->state = TCPDELAY_CAPTURE_DISABLED;
		return;
	}
	capture->epoch = next_epoch;
	reset_timing(capture, next_epoch);
	capture->state = TCPDELAY_CAPTURE_READY;
}

/*
 * Protocol 0 receives nothing until bind(), so no packet is queued before the
 * filter is attached. The filter accepts no packet, so the socket's receive
 * queue stays empty.
 */
static int attach_socket(
	struct tcpdelay_capture *capture,
	int program,
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
		    &program,
		    sizeof(program)
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

int tcpdelay_capture_open(
	struct tcpdelay_capture *capture,
	const char *object_path,
	const char *interface,
	const struct cake_accounting *accounting,
	struct tcpdelay_estimator *estimator,
	char *error,
	size_t error_size
)
{
	int program;
	int cpus;

	capture_clear(capture);
	capture->estimator = estimator;
	if (accounting != NULL) {
		capture->accounting.cake = *accounting;
		capture->accounting.enabled = 1U;
	}
	libbpf_set_print(forward_libbpf_message);

	capture->interface_index = if_nametoindex(interface);
	if (capture->interface_index == 0U)
		return error_set(
			error,
			error_size,
			"TCP delay interface %s: %s",
			interface,
			strerror(errno)
		);
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
	capture->fault_values = calloc(capture->cpu_count, PERCPU_VALUE_STRIDE);
	capture->lifetime = calloc(1U, sizeof(*capture->lifetime));
	if (capture->counter_values == NULL || capture->fault_values == NULL ||
	    capture->lifetime == NULL) {
		error_set(
			error,
			error_size,
			"could not allocate TCP delay counters: %s",
			strerror(errno)
		);
		goto fail;
	}
	tcpdelay_lifetime_init(capture->lifetime, 1U);
	program = load_program(capture, object_path, error, error_size);
	if (program < 0 || attach_socket(capture, program, interface, error, error_size) != 0)
		goto fail;
	return 0;

fail:
	tcpdelay_capture_close(capture);
	return -1;
}

int tcpdelay_capture_drain(struct tcpdelay_capture *capture)
{
	int count;

	if (capture->ring == NULL)
		return -1;
	(void)inspect_stream(capture);
	recover_stream(capture);
	count = ring_buffer__consume_n(capture->ring, TCPDELAY_STREAM_MAX_DRAIN);
	if (count < 0)
		return count;
	(void)inspect_stream(capture);
	recover_stream(capture);
	capture->ring_busy = ring__avail_data_size(ring_buffer__ring(capture->ring, 0U)) != 0U;
	return count;
}

bool tcpdelay_capture_timing_available(const struct tcpdelay_capture *capture)
{
	return capture->state == TCPDELAY_CAPTURE_READY && !capture->ring_busy &&
	       !capture->uncertain_pending;
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
		counters->ring_full += capture->counter_values[cpu].ring_full;
		counters->ack_bytes += capture->counter_values[cpu].ack_bytes;
		counters->upload_bytes += capture->counter_values[cpu].upload_bytes;
		counters->unaccounted_packets += capture->counter_values[cpu].unaccounted_packets;
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
	free(capture->fault_values);
	free(capture->lifetime);
	capture_clear(capture);
}
