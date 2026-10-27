#define _GNU_SOURCE

#include "daemon/report.h"
#include "daemon/state.h"

#include "common/clock.h"
#include "common/convert.h"
#include "logging/log.h"
#include "platform/ubus.h"
#include "constants/program.h"
#include "constants/states.h"
#include "constants/ubus.h"
#include "constants/units.h"

#include <libubus.h>
#include <libubox/blobmsg.h>

#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* What the status reply carries besides the snapshot; fixed while the object is served. */
struct publish_identity {
	char interface[DIRECTION_COUNT][IF_NAMESIZE];
	uint32_t pid;
	uint64_t started_sec;
};

static void
publish_direction(struct blob_buf *reply, const char *name, const struct status_direction *direction)
{
	void *table = blobmsg_open_table(reply, name);

	blobmsg_add_string(reply, STATUS_FIELD_INTERFACE, direction->interface);
	blobmsg_add_u8(reply, STATUS_FIELD_AVAILABLE, direction->available);
	if (direction->available) {
		blobmsg_add_u64(reply, STATUS_FIELD_SHAPER, direction->shaper_bps);
		blobmsg_add_u64(reply, STATUS_FIELD_ACHIEVED, direction->achieved_bps);
		blobmsg_add_u64(reply, STATUS_FIELD_LOAD, direction->load_e6);
		/* blobmsg prints INT64 as signed, so a negative delay survives. */
		blobmsg_add_u64(reply, STATUS_FIELD_DELAY, (uint64_t)direction->delay_us);
		blobmsg_add_u8(reply, STATUS_FIELD_BUFFERBLOAT, direction->bufferbloat);
		if (direction->tcp_queue_valid)
			blobmsg_add_u64(
				reply,
				STATUS_FIELD_TCP_QUEUE,
				(uint64_t)direction->tcp_queue_us
			);
	}
	blobmsg_close_table(reply, table);
}

/* Each direction's table in the status reply. */
static const char *const direction_fields[DIRECTION_COUNT] = {
	[DIRECTION_DOWNLOAD] = STATUS_FIELD_DOWNLOAD,
	[DIRECTION_UPLOAD] = STATUS_FIELD_UPLOAD,
};

/* The status reply, on the ubus thread; the snapshot's interface pointers are not used. */
static void publish_reply(struct blob_buf *reply, const void *snapshot_data, const void *context)
{
	const struct status_snapshot *snapshot = snapshot_data;
	const struct publish_identity *identity = context;
	blob_buf_init(reply, 0);
	blobmsg_add_string(reply, STATUS_FIELD_VERSION, PROGRAM_VERSION);
	blobmsg_add_u32(reply, STATUS_FIELD_PID, identity->pid);
	blobmsg_add_u64(reply, STATUS_FIELD_STARTED, identity->started_sec);
	blobmsg_add_string(reply, STATUS_FIELD_STATE, report_activity_name(snapshot->state));
	for (size_t index = 0U; index < DIRECTION_COUNT; index++) {
		struct status_direction direction = snapshot->directions[index];

		direction.interface = identity->interface[index];
		publish_direction(reply, direction_fields[index], &direction);
	}
}

/* The current status; left zeroed when the clock cannot be read. */
static struct status_snapshot publish_snapshot(struct daemon *daemon, bool *taken)
{
	struct status_snapshot snapshot = { 0 };
	uint64_t now_us;

	*taken = read_clock_us(CLOCK_MONOTONIC, &now_us);
	if (*taken)
		status_snapshot(daemon, now_us, &snapshot);
	return snapshot;
}

/*
 * Logs what the thread reported since the last tick, in the order the events
 * follow each other, with dnsmasq's message and level for the same ubus event.
 * The two info events go through log_system_message(), so they reach syslog
 * at info level, as dnsmasq logs them, and the daemon's log file. A refusal on
 * the first connection ends the run, as dnsmasq exits then; procd starts the
 * daemon again.
 */
static void publish_report(struct daemon *daemon, unsigned int events)
{
	const struct daemon_publish *publish = &daemon->publish;
	const struct ubus_server *server = publish->server;

	if (events & UBUS_SERVER_EVENT_DISCONNECTED)
		log_system_message("disconnecting from ubus");
	if (events & UBUS_SERVER_EVENT_RECONNECT_FAILED)
		log_message(
			LOG_LEVEL_ERROR,
			"cannot reconnect to ubus: %s",
			ubus_strerror(ubus_server_reconnect_status(server))
		);
	if (events & UBUS_SERVER_EVENT_NO_CONNECTION)
		log_message(LOG_LEVEL_ERROR, "cannot publish status on ubus: no connection");
	if (events & UBUS_SERVER_EVENT_CONNECTED)
		log_system_message(
			"connected to ubus; status published as %s",
			ubus_server_object_name(server)
		);
	if (events & UBUS_SERVER_EVENT_ADD_FAILED)
		log_message(
			LOG_LEVEL_ERROR,
			"ubus error: %s; status not published",
			ubus_strerror(ubus_server_add_status(server))
		);
	if (events & UBUS_SERVER_EVENT_ADD_FAILED_AT_START) {
		log_message(
			LOG_LEVEL_ERROR,
			"ubus error: %s; exiting",
			ubus_strerror(ubus_server_add_status(server))
		);
		daemon->result = -1;
		uloop_end();
	}
}

void publish_start(struct daemon *daemon, const char *section_name)
{
	struct daemon_publish *publish = &daemon->publish;
	const struct daemon_links *links = &daemon->links;
	char object_name[UBUS_OBJECT_NAME_SIZE];
	struct publish_identity identity = { .pid = (uint32_t)getpid() };
	struct status_snapshot snapshot;
	bool taken;
	uint64_t now_us;
	int error;

	(void)snprintf(object_name, sizeof(object_name), "%s.%s", UBUS_OBJECT_PREFIX, section_name);
	for (size_t index = 0U; index < DIRECTION_COUNT; index++) {
		const struct links_direction *direction = &links->directions[index];

		(void)snprintf(
			identity.interface[index],
			sizeof(identity.interface[index]),
			"%s",
			direction->interface
		);
	}
	if (read_clock_us(CLOCK_REALTIME, &now_us))
		identity.started_sec = now_us / MICROSECONDS_PER_SECOND;
	snapshot = publish_snapshot(daemon, &taken);
	publish->server = ubus_server_start(
		&(const struct ubus_server_settings){
			.object_name = object_name,
			.type_name = UBUS_OBJECT_PREFIX,
			.method = UBUS_METHOD_STATUS,
			.snapshot = &snapshot,
			.snapshot_size = sizeof(snapshot),
			.reply = publish_reply,
			.context = &identity,
			.context_size = sizeof(identity),
		},
		&error
	);
	if (publish->server == NULL)
		log_message(LOG_LEVEL_WARNING, "status not published on ubus: %s", strerror(error));
}

void publish_update(struct daemon *daemon)
{
	struct daemon_publish *publish = &daemon->publish;
	struct status_snapshot snapshot;
	bool taken;

	if (publish->server == NULL)
		return;
	snapshot = publish_snapshot(daemon, &taken);
	if (!taken)
		return;
	ubus_server_update(publish->server, &snapshot);
	publish_report(daemon, ubus_server_take_events(publish->server));
}

void publish_stop(struct daemon *daemon)
{
	struct daemon_publish *publish = &daemon->publish;
	struct ubus_server *server = publish->server;

	if (server == NULL)
		return;
	publish->server = NULL;
	if (ubus_server_stop(server, PUBLISH_STOP_US))
		return;
	log_message(
		LOG_LEVEL_WARNING,
		"ubus status thread did not stop within %.1f s; leaving it behind",
		us_to_sec_decimal(PUBLISH_STOP_US)
	);
}
