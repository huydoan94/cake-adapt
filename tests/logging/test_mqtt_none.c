/* The cake-adapt package's sender, built without MQTT: nothing is sent, and asking for MQTT fails. */
#include "config/config.h"
#include "logging/mqtt.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

int main(void)
{
	struct mqtt_settings settings = { .enabled = false };
	char error[256] = "";

	assert(mqtt_start(&settings, error, sizeof(error)) == 0);
	assert(!mqtt_enabled());
	settings.enabled = true;
	errno = 0;
	assert(mqtt_start(&settings, error, sizeof(error)) == -1);
	assert(errno == ENOTSUP);
	assert(strcmp(
		       error,
		       "this package was built without MQTT support; install cake-adapt-mqtt or cake-adapt-mqtt-ssl"
	       ) == 0);
	assert(!mqtt_enabled());
	mqtt_submit("line");
	mqtt_stop();
	assert(!mqtt_enabled());
	return 0;
}
