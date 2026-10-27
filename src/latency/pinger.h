#ifndef LATENCY_PINGER_H_INCLUDED
#define LATENCY_PINGER_H_INCLUDED

/* pinger.c: the pinger methods, what each runs and how it measures. */

#include "latency/icmp.h"

#include <stdbool.h>
#include <stddef.h>

struct child_ops;

/*
 * The pinger_method values, looked up once from the option and passed around;
 * the functions below take a known method, never PINGER_METHOD_UNKNOWN.
 */
enum pinger_method {
	PINGER_METHOD_NATIVE,
	PINGER_METHOD_NATIVE_TS,
	PINGER_METHOD_FPING,
	PINGER_METHOD_FPING_TS,
	PINGER_METHOD_IRTT,
	PINGER_METHOD_UNKNOWN,
};

enum pinger_method pinger_method_parse(const char *name);
const char *pinger_method_name(enum pinger_method method);
/* The daemon's own ICMP probes, with no executable. */
bool pinger_is_native(enum pinger_method method);
/* The executable a method runs, or NULL for the native probes. */
const char *pinger_executable(enum pinger_method method);
/* Echo methods report half the round trip as the delay of both directions. */
bool pinger_shared_delay(enum pinger_method method);
/* ICMP timestamp methods, IPv4 only. */
bool pinger_uses_timestamps(enum pinger_method method);

/* Like cake-autorate, refuse to start when the selected pinger cannot run. */
int pinger_check(enum pinger_method method, char *error, size_t error_size);

/* For the session: a child pinger's operations (NULL for native) and the native probes' packets. */
const struct child_ops *pinger_child_ops(enum pinger_method method);
enum icmp_mode pinger_icmp_mode(enum pinger_method method);

#endif
