#ifndef LOGGING_MQTT_H_INCLUDED
#define LOGGING_MQTT_H_INCLUDED

#include "constants/options.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the sender needs from the configuration: the broker and its credentials. */
struct mqtt_settings {
	bool enabled;
	bool tls;
	char host[CONFIG_STRING_SIZE];
	char topic[CONFIG_STRING_SIZE];
	char username[CONFIG_STRING_SIZE];
	char password[CONFIG_STRING_SIZE];
	char ca_file[CONFIG_STRING_SIZE];
	uint64_t port;
};

/*
 * The main thread owns start/stop and submit. Network work stays in the sender. A failed
 * start returns -1 with the failing step in error.
 */
int mqtt_start(const struct mqtt_settings *settings, char *error, size_t error_size);
void mqtt_stop(void);
bool mqtt_enabled(void);
void mqtt_submit(const char *line);

#endif
