#ifndef MQTT_H_INCLUDED
#define MQTT_H_INCLUDED

#include <stdbool.h>

struct config_mqtt;

/* The main thread owns start/stop and submit. Network work stays in the sender. */
int mqtt_start(const struct config_mqtt *settings);
void mqtt_stop(void);
bool mqtt_enabled(void);
void mqtt_submit(const char *line);

#endif
