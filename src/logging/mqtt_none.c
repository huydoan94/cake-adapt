/*
 * The sender of the package built without MQTT (MQTT_SUPPORTED=0): it links no libmosquitto.
 * A start with mqtt_enabled on fails, so main() logs a warning, and the instance runs without
 * sending anything.
 */
#include "logging/mqtt.h"
#include "config/config.h"
#include "common/error.h"

#include <errno.h>

int mqtt_start(const struct config_mqtt *settings, char *error, size_t error_size)
{
	if (!settings->enabled)
		return 0;
	errno = ENOTSUP;
	return error_set(
		error,
		error_size,
		"this package was built without MQTT support; install cake-adapt-mqtt or cake-adapt-mqtt-ssl"
	);
}

void mqtt_stop(void)
{
}

bool mqtt_enabled(void)
{
	return false;
}

void mqtt_submit(const char *line)
{
	(void)line;
}
