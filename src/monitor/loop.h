#ifndef MONITOR_LOOP_H_INCLUDED
#define MONITOR_LOOP_H_INCLUDED

/* Private to the monitor: event-loop state shared by its source files. */

#include "monitor/control.h"
#include "monitor/links.h"
#include "monitor/pingers.h"
#include "monitor/publish.h"
#include "monitor/reflectors.h"
#include "monitor/status.h"
#include "monitor/tcpdelay.h"
#include "monitor/upkeep.h"
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
struct monitor {
	const struct config *config;
	/* config->pinger_method, looked up once. */
	enum pinger_method pinger_method;
	/* Shared by links.c (discovery, events) and control.c (bandwidth changes). */
	struct netlink netlink;
	struct monitor_links links;
	struct monitor_control control;
	struct monitor_pingers pingers;
	struct monitor_reflectors reflectors;
	struct monitor_tcp tcp;
	struct monitor_publish publish;
	struct monitor_upkeep upkeep;
	/* monitor.c: the traffic tick and the activity state. */
	struct controller_activity activity;
	struct uloop_interval traffic_timer;
	int result;
};

/*
 * An observation's failure and recovery, each logged once: the warning on the
 * first failure, the notice when a failed observation works again.
 */
static inline void monitor_degraded(bool *failed, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

static inline void monitor_degraded(bool *failed, const char *format, ...)
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

static inline void monitor_recovered(bool *failed, const char *message)
{
	if (!*failed)
		return;
	*failed = false;
	log_message(LOG_LEVEL_NOTICE, "%s", message);
}

/* monitor.c */

void monitor_tick(struct monitor *monitor);

#endif
