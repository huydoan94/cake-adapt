#define _GNU_SOURCE

#include "tcpdelay/injector.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/error.h"

/* Names in tcpdelay.bpf.c: the injector program and its maps. */
#define INJECT_PROGRAM "inject_egress"
#define INJECT_COUNTERS "inject_counters"
#define INJECT_SETTINGS "inject_settings"
#define INJECT_STATE "inject_state"

void tcpdelay_injector_init(struct tcpdelay_injector *injector)
{
	memset(injector, 0, sizeof(*injector));
	injector->counters_descriptor = -1;
}

/* Where the IP header starts in the packets the program sees on interface. */
static int network_offset(const char *interface, uint32_t *offset, char *error, size_t error_size)
{
	struct ifreq request = { 0 };
	int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int failure = 0;

	(void)snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
	if (descriptor < 0 || ioctl(descriptor, SIOCGIFHWADDR, &request) != 0) {
		failure = errno;
	} else if (request.ifr_hwaddr.sa_family == ARPHRD_ETHER) {
		*offset = ETH_HLEN;
	} else if (request.ifr_hwaddr.sa_family == ARPHRD_PPP ||
		   request.ifr_hwaddr.sa_family == ARPHRD_NONE ||
		   request.ifr_hwaddr.sa_family == ARPHRD_RAWIP) {
		/* Devices without a link header, such as PPPoE's ppp device. */
		*offset = 0U;
	} else {
		failure = EOPNOTSUPP;
	}
	if (descriptor >= 0)
		(void)close(descriptor);
	if (failure == 0)
		return 0;
	return error_set(
		error,
		error_size,
		"TCP timestamp injection cannot use interface %s: %s",
		interface,
		strerror(failure)
	);
}

/* Whether a map of the capture's object has the value size this daemon uses. */
static bool layout_matches(const struct tcpdelay_capture *capture, const char *name, size_t size)
{
	const struct bpf_map *map = bpf_object__find_map_by_name(capture->object, name);

	return map != NULL && bpf_map__value_size(map) == size;
}

/* Finds the counters and allocates their per-CPU copies, once. */
static int find_counters(
	struct tcpdelay_injector *injector,
	const struct tcpdelay_capture *capture,
	char *error,
	size_t error_size
)
{
	int cpus;

	if (injector->counter_values != NULL)
		return 0;
	cpus = libbpf_num_possible_cpus();
	injector->counters_descriptor =
		bpf_object__find_map_fd_by_name(capture->object, INJECT_COUNTERS);
	if (cpus <= 0 || injector->counters_descriptor < 0)
		return error_set(
			error,
			error_size,
			"TCP timestamp injection counters are unavailable"
		);
	injector->counter_values = calloc((size_t)cpus, sizeof(*injector->counter_values));
	if (injector->counter_values == NULL)
		return error_set(
			error,
			error_size,
			"could not allocate TCP timestamp injection counters: %s",
			strerror(errno)
		);
	injector->cpu_count = (size_t)cpus;
	return 0;
}

int tcpdelay_injector_attach(
	struct tcpdelay_injector *injector,
	const struct tcpdelay_capture *capture,
	const char *interface,
	char *error,
	size_t error_size
)
{
	struct tcpdelay_inject_settings values = { 0 };
	struct bpf_program *program = NULL;
	unsigned int interface_index;
	uint32_t key = 0U;

	tcpdelay_injector_detach(injector);
	if (capture->object != NULL && capture->injection)
		program = bpf_object__find_program_by_name(capture->object, INJECT_PROGRAM);
	if (program == NULL || bpf_program__fd(program) < 0)
		return error_set(error, error_size, "TCP timestamp injection program is not loaded");
	/*
	 * The daemon and the object must agree on the layouts it reads and writes;
	 * a mismatch, such as an object from another build, would corrupt memory.
	 */
	if (!layout_matches(capture, INJECT_COUNTERS, sizeof(struct tcpdelay_inject_counters)) ||
	    !layout_matches(capture, INJECT_STATE, sizeof(struct tcpdelay_inject_state)))
		return error_set(
			error,
			error_size,
			"TCP timestamp injection object does not match this daemon"
		);
	if (find_counters(injector, capture, error, error_size) != 0 ||
	    network_offset(interface, &values.network_offset, error, error_size) != 0)
		return -1;
	interface_index = if_nametoindex(interface);
	if (interface_index == 0U ||
	    bpf_map_update_elem(
		    bpf_object__find_map_fd_by_name(capture->object, INJECT_SETTINGS),
		    &key,
		    &values,
		    BPF_ANY
	    ) != 0)
		return error_set(
			error,
			error_size,
			"could not configure TCP timestamp injection on %s: %s",
			interface,
			strerror(errno)
		);
	injector->egress = bpf_program__attach_tcx(program, (int)interface_index, NULL);
	if (injector->egress == NULL)
		return error_set(
			error,
			error_size,
			"could not attach TCP timestamp injection to %s: %s",
			interface,
			strerror(errno)
		);
	return 0;
}

bool tcpdelay_injector_attached(const struct tcpdelay_injector *injector)
{
	return injector->egress != NULL;
}

int tcpdelay_injector_counters(
	const struct tcpdelay_injector *injector,
	struct tcpdelay_inject_counters *counters
)
{
	uint32_t key = 0U;
	size_t cpu;

	memset(counters, 0, sizeof(*counters));
	if (injector->counter_values == NULL ||
	    bpf_map_lookup_elem(injector->counters_descriptor, &key, injector->counter_values) != 0)
		return -1;
	for (cpu = 0U; cpu < injector->cpu_count; cpu++) {
		const struct tcpdelay_inject_counters *value = &injector->counter_values[cpu];

		counters->injected += value->injected;
		counters->skipped += value->skipped;
		counters->server_accepted += value->server_accepted;
		counters->server_declined += value->server_declined;
		counters->client_rejected += value->client_rejected;
		counters->server_rejected += value->server_rejected;
		counters->retried += value->retried;
		counters->failed += value->failed;
		counters->stalled_ipv4 += value->stalled_ipv4;
		counters->stalled_ipv6 += value->stalled_ipv6;
		counters->paused += value->paused;
	}
	return 0;
}

int tcpdelay_injector_state(
	const struct tcpdelay_capture *capture,
	struct tcpdelay_inject_state *state
)
{
	uint32_t key = 0U;
	int descriptor;

	if (capture->object == NULL)
		return -1;
	descriptor = bpf_object__find_map_fd_by_name(capture->object, INJECT_STATE);
	if (descriptor < 0 || bpf_map_lookup_elem(descriptor, &key, state) != 0)
		return -1;
	return 0;
}

int tcpdelay_injector_set_state(
	const struct tcpdelay_capture *capture,
	const struct tcpdelay_inject_state *state
)
{
	uint32_t key = 0U;
	int descriptor;

	if (capture->object == NULL)
		return -1;
	descriptor = bpf_object__find_map_fd_by_name(capture->object, INJECT_STATE);
	if (descriptor < 0 || bpf_map_update_elem(descriptor, &key, state, BPF_ANY) != 0)
		return -1;
	return 0;
}

void tcpdelay_injector_detach(struct tcpdelay_injector *injector)
{
	(void)bpf_link__destroy(injector->egress);
	injector->egress = NULL;
}

void tcpdelay_injector_unload(struct tcpdelay_injector *injector)
{
	tcpdelay_injector_detach(injector);
	free(injector->counter_values);
	tcpdelay_injector_init(injector);
}
