#define _GNU_SOURCE

#include "cake/cake.h"
#include "common/constants.h"
#include "common/error.h"
#include "common/utils.h"

#include <errno.h>
#include <limits.h>
#include <linux/gen_stats.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct cake_read_context {
	struct cake_read *reads;
	size_t count;
};

static void parse_options(struct nlattr *options, struct cake_observation *observation)
{
	static const struct nla_policy policy[TCA_CAKE_MAX + 1] = {
		[TCA_CAKE_BASE_RATE64] = { .type = NLA_U64 },
		[TCA_CAKE_ATM] = { .type = NLA_U32 },
		[TCA_CAKE_OVERHEAD] = { .type = NLA_S32 },
		[TCA_CAKE_RAW] = { .type = NLA_U32 }
	};
	struct nlattr *attributes[TCA_CAKE_MAX + 1];

	if (options == NULL || nla_parse_nested(attributes, TCA_CAKE_MAX, options, policy) < 0)
		return;
	if (attributes[TCA_CAKE_BASE_RATE64] != NULL) {
		observation->bandwidth_bits_per_second = saturating_mul(
			nla_get_u64(attributes[TCA_CAKE_BASE_RATE64]),
			BITS_PER_BYTE
		);
		observation->has_bandwidth = true;
	}
	if (attributes[TCA_CAKE_ATM] != NULL)
		observation->atm_mode = nla_get_u32(attributes[TCA_CAKE_ATM]);
	if (attributes[TCA_CAKE_OVERHEAD] != NULL)
		observation->overhead_bytes = nla_get_s32(attributes[TCA_CAKE_OVERHEAD]);
	if (attributes[TCA_CAKE_RAW] != NULL)
		observation->raw = true;
}

uint64_t cake_max_wire_packet_bits(const struct cake_observation *observation)
{
	uint64_t bits;

	if (!observation->has_mtu)
		return 0U;
	bits = saturating_mul(observation->mtu_bytes, BITS_PER_BYTE);
	if (observation->raw || observation->overhead_bytes < 0 ||
	    (observation->atm_mode != CAKE_ATM_NONE && observation->atm_mode != CAKE_ATM_ATM)) {
		return bits;
	}
	bits = saturating_mul(
		saturating_add(observation->mtu_bytes, (uint64_t)observation->overhead_bytes),
		BITS_PER_BYTE
	);
	if (observation->atm_mode != CAKE_ATM_ATM)
		return bits;
	/* Whole 48-byte ATM cell payloads, each sent as a 53-byte cell. */
	return saturating_mul(saturating_add(bits, UINT64_C(376)) / UINT64_C(384), UINT64_C(424));
}

static void parse_cake_stats(struct nlattr *application, struct cake_observation *observation)
{
	static const struct nla_policy policy[TCA_CAKE_STATS_MAX + 1] = {
		[TCA_CAKE_STATS_CAPACITY_ESTIMATE64] = { .type = NLA_U64 },
		[TCA_CAKE_STATS_MEMORY_LIMIT] = { .type = NLA_U32 },
		[TCA_CAKE_STATS_MEMORY_USED] = { .type = NLA_U32 }
	};
	struct nlattr *attributes[TCA_CAKE_STATS_MAX + 1];

	if (application == NULL ||
	    nla_parse_nested(attributes, TCA_CAKE_STATS_MAX, application, policy) < 0) {
		return;
	}
	if (attributes[TCA_CAKE_STATS_CAPACITY_ESTIMATE64] != NULL) {
		observation->capacity_estimate_bits_per_second = saturating_mul(
			nla_get_u64(attributes[TCA_CAKE_STATS_CAPACITY_ESTIMATE64]),
			BITS_PER_BYTE
		);
	}
	if (attributes[TCA_CAKE_STATS_MEMORY_LIMIT] != NULL) {
		observation->memory_limit_bytes =
			nla_get_u32(attributes[TCA_CAKE_STATS_MEMORY_LIMIT]);
	}
	if (attributes[TCA_CAKE_STATS_MEMORY_USED] != NULL) {
		observation->memory_used_bytes =
			nla_get_u32(attributes[TCA_CAKE_STATS_MEMORY_USED]);
	}
}

static void parse_stats(struct nlattr *stats, struct cake_observation *observation)
{
	static const struct nla_policy policy[TCA_STATS_MAX + 1] = {
		/* The ABI is 12 bytes; sizeof(gnet_stats_basic) can include padding. */
		[TCA_STATS_BASIC] = { .type = NLA_BINARY,
				      .minlen = sizeof(uint64_t) + sizeof(uint32_t) },
		[TCA_STATS_QUEUE] = { .type = NLA_BINARY,
				      .minlen = sizeof(struct gnet_stats_queue) }
	};
	struct nlattr *attributes[TCA_STATS_MAX + 1];

	/* Invalid optional statistics do not prevent discovering the qdisc. */
	if (stats == NULL || nla_parse_nested(attributes, TCA_STATS_MAX, stats, policy) < 0)
		return;
	if (attributes[TCA_STATS_BASIC] != NULL) {
		struct gnet_stats_basic basic = { 0 };

		nla_memcpy(&basic, attributes[TCA_STATS_BASIC], sizeof(basic));
		observation->bytes = basic.bytes;
		observation->packets = basic.packets;
		observation->has_basic_stats = true;
	}
	if (attributes[TCA_STATS_QUEUE] != NULL) {
		struct gnet_stats_queue queue;

		nla_memcpy(&queue, attributes[TCA_STATS_QUEUE], sizeof(queue));
		observation->queue_length = queue.qlen;
		observation->backlog_bytes = queue.backlog;
		observation->drops = queue.drops;
	}
	parse_cake_stats(attributes[TCA_STATS_APP], observation);
}

static int handle_qdisc(const struct nlmsghdr *message, void *context_pointer)
{
	struct cake_read_context *context = context_pointer;
	static const struct nla_policy policy[TCA_MAX + 1] = { [TCA_KIND] = {
								       .type = NLA_NUL_STRING } };
	const struct tcmsg *traffic_control;
	struct nlattr *attributes[TCA_MAX + 1];
	struct cake_read *read = NULL;
	size_t index;

	if (!nlmsg_valid_hdr(message, sizeof(struct tcmsg)) || message->nlmsg_len > INT_MAX)
		return -1;

	traffic_control = NLMSG_DATA(message);
	/* Only control the interface's root CAKE, never a nested child qdisc. */
	if (traffic_control->tcm_parent != TC_H_ROOT)
		return 0;
	for (index = 0U; index < context->count; index++) {
		if (!context->reads[index].found && context->reads[index].interface_index != 0U &&
		    traffic_control->tcm_ifindex == (int)context->reads[index].interface_index) {
			read = &context->reads[index];
			break;
		}
	}
	if (read == NULL)
		return 0;

	if (nla_parse(
		    attributes,
		    TCA_MAX,
		    nlmsg_attrdata(message, sizeof(*traffic_control)),
		    nlmsg_attrlen(message, sizeof(*traffic_control)),
		    policy
	    ) < 0 ||
	    attributes[TCA_KIND] == NULL || nla_strcmp(attributes[TCA_KIND], QDISC_KIND) != 0) {
		return 0;
	}

	memset(read->observation, 0, sizeof(*read->observation));
	read->observation->interface_index = read->interface_index;
	read->observation->handle = traffic_control->tcm_handle;
	read->observation->parent = traffic_control->tcm_parent;
	parse_options(attributes[TCA_OPTIONS], read->observation);
	parse_stats(attributes[TCA_STATS2], read->observation);
	read->found = true;
	return 0;
}

static int
read_interface_mtu(const char *interface, uint32_t *mtu_bytes, char *error, size_t error_size)
{
	struct ifreq request = { 0 };
	int socket_fd;

	if (strlen(interface) >= sizeof(request.ifr_name))
		return error_set(error, error_size, "interface name is too long");
	socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (socket_fd < 0) {
		return error_set(
			error,
			error_size,
			"could not open interface control socket: %s",
			strerror(errno)
		);
	}
	memcpy(request.ifr_name, interface, strlen(interface) + 1U);
	if (ioctl(socket_fd, SIOCGIFMTU, &request) != 0) {
		error_set(
			error,
			error_size,
			"could not read interface MTU for %s: %s",
			interface,
			strerror(errno)
		);
		(void)close(socket_fd);
		return -1;
	}
	if (request.ifr_mtu <= 0) {
		error_set(error, error_size, "interface MTU for %s is invalid", interface);
		(void)close(socket_fd);
		return -1;
	}
	(void)close(socket_fd);
	*mtu_bytes = (uint32_t)request.ifr_mtu;
	return 0;
}

static void finish_read(struct cake_read *read)
{
	struct cake_observation *observation = read->observation;

	/* The dump cannot reject a stale index, so keep it only while CAKE is found. */
	if (!read->found) {
		observation->interface_index = 0U;
		observation->has_mtu = false;
		read->result = CAKE_READ_NOT_FOUND;
		return;
	}
	/* The MTU is read when CAKE is discovered or replaced, not every sample. */
	if (read->previous.has_mtu &&
	    read->previous.interface_index == observation->interface_index &&
	    read->previous.handle == observation->handle) {
		observation->mtu_bytes = read->previous.mtu_bytes;
	} else if (read_interface_mtu(
			   read->interface,
			   &observation->mtu_bytes,
			   read->error,
			   sizeof(read->error)
		   ) != 0) {
		read->result = CAKE_READ_ERROR;
		return;
	}
	observation->has_mtu = true;
	read->result = CAKE_READ_FOUND;
}

void cake_read_all(struct netlink *netlink, struct cake_read *reads, size_t count)
{
	struct cake_read_context context = { .reads = reads, .count = count };
	char error[ERROR_SIZE] = { 0 };
	bool pending = false;
	size_t index;

	for (index = 0U; index < count; index++) {
		struct cake_read *read = &reads[index];

		read->previous = *read->observation;
		read->interface_index = read->previous.interface_index;
		read->found = false;
		read->error[0] = '\0';
		if (read->interface_index == 0U) {
			errno = 0;
			read->interface_index = if_nametoindex(read->interface);
		}
		if (read->interface_index == 0U) {
			error_set(
				read->error,
				sizeof(read->error),
				"could not find interface '%s': %s",
				read->interface,
				errno == 0 ? "unknown interface" : strerror(errno)
			);
			read->result = CAKE_READ_ERROR;
			continue;
		}
		pending = true;
	}
	if (!pending)
		return;

	/* The kernel ignores tcm_ifindex, so one dump serves every interface. */
	if (netlink_open(netlink, error, sizeof(error)) != 0 ||
	    netlink_dump_qdiscs(netlink, handle_qdisc, &context, error, sizeof(error)) != 0) {
		netlink_close_requests(netlink);
		for (index = 0U; index < count; index++) {
			if (reads[index].interface_index != 0U) {
				reads[index].observation->interface_index = 0U;
				reads[index].result = CAKE_READ_ERROR;
				(void)snprintf(
					reads[index].error,
					sizeof(reads[index].error),
					"%s",
					error
				);
			}
		}
		return;
	}
	for (index = 0U; index < count; index++)
		if (reads[index].interface_index != 0U)
			finish_read(&reads[index]);
}

enum cake_read_result cake_read(
	struct netlink *netlink,
	const char *interface,
	struct cake_observation *observation,
	char *error,
	size_t error_size
)
{
	struct cake_read read = { .interface = interface, .observation = observation };

	cake_read_all(netlink, &read, 1U);
	if (read.result == CAKE_READ_ERROR)
		error_set(error, error_size, "%s", read.error);
	return read.result;
}

int cake_set_bandwidth(
	struct netlink *netlink,
	const struct cake_observation *observation,
	uint64_t bandwidth_bits_per_second,
	char *error,
	size_t error_size
)
{
	uint64_t bandwidth_bytes_per_second;

	if (bandwidth_bits_per_second < 8U || bandwidth_bits_per_second % 8U != 0U) {
		return error_set(
			error,
			error_size,
			"CAKE bandwidth must be a positive multiple of 8 bit/s"
		);
	}
	if (netlink_open(netlink, error, error_size) != 0)
		return -1;

	bandwidth_bytes_per_second = bandwidth_bits_per_second / 8U;
	if (netlink_change_qdisc_option(
		    netlink,
		    observation->interface_index,
		    observation->handle,
		    observation->parent,
		    QDISC_KIND,
		    TCA_CAKE_BASE_RATE64,
		    &bandwidth_bytes_per_second,
		    sizeof(bandwidth_bytes_per_second),
		    error,
		    error_size
	    ) != 0) {
		netlink_close_requests(netlink);
		return -1;
	}

	return 0;
}
