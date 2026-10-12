/* A command-driven fixture uses the production sender and logging paths. */
#include "logging/mqtt.c"
#include "logging/log.h"

#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	struct config_mqtt settings = {
		.enabled = true,
		.host = "localhost",
		.topic = "cake-adapt/test",
	};
	char *line = NULL;
	size_t capacity = 0U;
	bool foreground = false;
	uint64_t before_us;
	uint64_t after_us;

	assert(argc >= 2);
	settings.port = strtoull(argv[1], NULL, 10);
	if (argc >= 4) {
		(void)snprintf(settings.username, sizeof(settings.username), "%s", argv[2]);
		(void)snprintf(settings.password, sizeof(settings.password), "%s", argv[3]);
	}
	log_init("mqtt-test", foreground);
	assert(mqtt_start(&settings) == 0);
	(void)puts("ready");
	(void)fflush(stdout);
	while (getline(&line, &capacity, stdin) >= 0) {
		line[strcspn(line, "\n")] = '\0';
		if (strcmp(line, "state") == 0) {
			(void)printf("%u\n", atomic_load(&sender->online) ? 1U : 0U);
		} else if (strncmp(line, "record ", 7U) == 0) {
			mqtt_submit(line + 7U);
			(void)puts("sent");
		} else if (strncmp(line, "message ", 8U) == 0) {
			log_message(LOG_LEVEL_INFO, "%s", line + 8U);
			(void)puts("sent");
		} else if (strcmp(line, "burst") == 0) {
			assert(read_clock_us(CLOCK_MONOTONIC, &before_us));
			for (unsigned int index = 0U; index < 10000U; index++)
				mqtt_submit("BURST; test record");
			assert(read_clock_us(CLOCK_MONOTONIC, &after_us));
			(void)printf("%llu\n", (unsigned long long)(after_us - before_us));
		} else if (strcmp(line, "stop") == 0) {
			assert(read_clock_us(CLOCK_MONOTONIC, &before_us));
			log_close();
			assert(read_clock_us(CLOCK_MONOTONIC, &after_us));
			(void)printf("%llu\n", (unsigned long long)(after_us - before_us));
			(void)fflush(stdout);
			break;
		}
		(void)fflush(stdout);
	}
	free(line);
	log_close();
	return 0;
}
