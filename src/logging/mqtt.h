#ifndef MQTT_H_INCLUDED
#define MQTT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>

struct config_mqtt;

/*
 * The main thread owns start/stop and submit. Network work stays in the sender. A failed
 * start returns -1 with the failing step in error.
 */
int mqtt_start(const struct config_mqtt *settings, char *error, size_t error_size);
void mqtt_stop(void);
bool mqtt_enabled(void);
void mqtt_submit(const char *line);

#endif
