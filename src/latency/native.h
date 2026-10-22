#ifndef LATENCY_NATIVE_H_INCLUDED
#define LATENCY_NATIVE_H_INCLUDED

#include "latency/icmp.h"
#include "latency/parser.h"
#include "common/constants.h"

struct native_request {
	uint64_t serial;
	uint64_t sent_us;
	uint32_t originate_ms;
	size_t target;
	bool pending;
};

struct native_target {
	const char *name;
	struct icmp_address address;
	uint64_t next_send_us;
	bool send_failed;
};

/* One raw socket per address family, opened only when a reflector needs it. */
enum native_channel {
	NATIVE_CHANNEL_IPV4,
	NATIVE_CHANNEL_IPV6,
	NATIVE_CHANNELS
};

struct native_session {
	struct icmp_socket channels[NATIVE_CHANNELS];
	struct native_target targets[CONFIG_MAX_REFLECTORS];
	struct native_request *requests;
	size_t capacity;
	size_t target_count;
	size_t next_target;
	enum icmp_mode mode;
	uint64_t token;
	uint64_t next_serial;
	uint64_t expired_serial;
	uint64_t period_us;
	uint64_t spacing_us;
	uint64_t next_send_us;
	uint64_t timeout_count;
	bool timestamp_warned;
};

bool native_method(const char *method);
bool native_send_failed(const struct native_session *session);
void native_init(struct native_session *session);
int native_open(
	struct native_session *session,
	const char *method,
	const char *interface,
	const char *const *targets,
	size_t target_count,
	uint64_t interval_us,
	uint64_t now_us,
	char *error,
	size_t error_size
);
void native_close(struct native_session *session);
uint64_t native_next_event(const struct native_session *session);
/* At most one send; missed slots are skipped. Returns 1 for a failed send slot,
 * already advanced so other targets continue; 0 otherwise. */
int native_tick(
	struct native_session *session,
	uint64_t now_us,
	uint64_t realtime_us,
	char *error,
	size_t error_size
);
/* The sample keeps realtime timestamp_us, as existing logging/tracker callers expect. */
bool native_accept(
	struct native_session *session,
	const struct icmp_reply *reply,
	uint64_t now_us,
	uint64_t realtime_us,
	struct latency_sample *sample,
	uint64_t *response_us
);

#endif
