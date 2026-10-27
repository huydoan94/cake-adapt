#define _GNU_SOURCE

#include "latency/pinger.h"
#include "latency/children.h"
#include "common/error.h"
#include "common/arith.h"
#include "constants/latency.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Every pinger_method: what it runs and how it measures. */
static const struct {
	const char *name;
	/* NULL for the native probes, which the daemon sends itself. */
	const char *executable;
	const struct child_ops *ops;
	/* The native probes' packets; echo unless timestamps. */
	enum icmp_mode mode;
	bool shared_delay;
	bool timestamps;
} methods[] = {
	[PINGER_METHOD_NATIVE] = { .name = PINGER_NAME_NATIVE, .shared_delay = true },
	[PINGER_METHOD_NATIVE_TS] = {
		.name = PINGER_NAME_NATIVE_TS,
		.mode = ICMP_MODE_TIMESTAMP,
		.timestamps = true,
	},
	[PINGER_METHOD_FPING] = {
		.name = PINGER_NAME_FPING,
		.executable = FPING_PATH,
		.ops = &fping_ops,
		.shared_delay = true,
	},
	[PINGER_METHOD_FPING_TS] = {
		.name = PINGER_NAME_FPING_TS,
		.executable = FPING_PATH,
		.ops = &fping_ts_ops,
		.timestamps = true,
	},
	[PINGER_METHOD_IRTT] = { .name = PINGER_NAME_IRTT, .executable = IRTT_PATH, .ops = &irtt_ops },
};

enum pinger_method pinger_method_parse(const char *name)
{
	for (size_t index = 0U; index < ARRAY_SIZE(methods); index++)
		if (strcmp(methods[index].name, name) == 0)
			return (enum pinger_method)index;
	return PINGER_METHOD_UNKNOWN;
}

const char *pinger_method_name(enum pinger_method method)
{
	return methods[method].name;
}

bool pinger_is_native(enum pinger_method method)
{
	return methods[method].executable == NULL;
}

bool pinger_shared_delay(enum pinger_method method)
{
	return methods[method].shared_delay;
}

bool pinger_uses_timestamps(enum pinger_method method)
{
	return methods[method].timestamps;
}

const char *pinger_executable(enum pinger_method method)
{
	return methods[method].executable;
}

int pinger_check(enum pinger_method method, char *error, size_t error_size)
{
	const char *executable = methods[method].executable;

	if (executable != NULL && access(executable, X_OK) != 0) {
		return error_set(
			error,
			error_size,
			"ping binary %s for pinger_method '%s' is not available: %s; install the %s package or use pinger_method '%s'",
			executable,
			methods[method].name,
			strerror(errno),
			strrchr(executable, '/') + 1,
			PINGER_NAME_NATIVE
		);
	}
	return 0;
}

const struct child_ops *pinger_child_ops(enum pinger_method method)
{
	return methods[method].ops;
}

enum icmp_mode pinger_icmp_mode(enum pinger_method method)
{
	return methods[method].mode;
}
