#include "logging/mqtt.h"

#include <stdio.h>

bool test_mqtt_active;
unsigned int test_mqtt_count;
char test_mqtt_line[2048];

bool mqtt_enabled(void)
{
	return test_mqtt_active;
}

void mqtt_submit(const char *line)
{
	if (!test_mqtt_active)
		return;
	test_mqtt_count++;
	(void)snprintf(test_mqtt_line, sizeof(test_mqtt_line), "%s", line);
}

void mqtt_stop(void)
{
	test_mqtt_active = false;
}
