#ifndef DAEMON_STATE_H_INCLUDED
#define DAEMON_STATE_H_INCLUDED

/* Private to the daemon: event-loop state shared by its source files. */

#include "daemon/shaper.h"
#include "daemon/links.h"
#include "daemon/pingers.h"
#include "daemon/publish.h"
#include "daemon/reflectors.h"
#include "daemon/status.h"
#include "daemon/tcp.h"
#include "daemon/upkeep.h"
#include "config/config.h"
#include "controller/activity.h"
#include "controller/controller.h"
#include "latency/latency.h"
#include "logging/log.h"
#include "platform/netlink.h"
#include "constants/text.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

/* The whole daemon state; each part belongs to the file its header names. */
struct daemon {
	const struct config *config;
	/* config->pinger_method, looked up once. */
	enum pinger_method pinger_method;
	/* Shared by links.c (discovery, events) and shaper.c (bandwidth changes). */
	struct netlink netlink;
	struct daemon_links links;
	struct daemon_shaper shaper;
	struct daemon_pingers pingers;
	struct daemon_reflectors reflectors;
	struct daemon_tcp tcp;
	struct daemon_publish publish;
	struct daemon_upkeep upkeep;
	/* daemon.c: the traffic tick and the activity state. */
	struct controller_activity activity;
	struct uloop_interval traffic_timer;
	int result;
};

/*
 * An observation's failure and recovery, each logged once: the warning on the
 * first failure, the notice when a failed observation works again.
 */
static inline void daemon_degraded(bool *failed, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

static inline void daemon_degraded(bool *failed, const char *format, ...)
{
	char message[ERROR_SIZE];
	va_list arguments;

	if (*failed)
		return;
	*failed = true;
	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	log_message(LOG_LEVEL_WARNING, "%s", message);
}

static inline void daemon_recovered(bool *failed, const char *message)
{
	if (!*failed)
		return;
	*failed = false;
	log_message(LOG_LEVEL_NOTICE, "%s", message);
}

/* daemon.c */

void daemon_tick(struct daemon *daemon);

#endif
