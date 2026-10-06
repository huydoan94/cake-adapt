#define _POSIX_C_SOURCE 200809L

#include "platform/memory.h"

#include "common/constants.h"
#include "common/error.h"
#include "common/utils.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_RSS "VmRSS"
#define FIELD_PEAK_RSS "VmHWM"
#define FIELD_ANONYMOUS "RssAnon"
#define FIELD_DATA "VmData"
/* Longer than any field name in /proc/<pid>/status; the scan below reads 31. */
#define FIELD_NAME_SIZE 32U

int memory_read(const char *path, struct memory_sample *sample, char *error, size_t error_size)
{
	const struct {
		const char *name;
		uint64_t *value;
	} fields[] = {
		{ FIELD_RSS, &sample->rss_kilobytes },
		{ FIELD_PEAK_RSS, &sample->peak_rss_kilobytes },
		{ FIELD_ANONYMOUS, &sample->anonymous_kilobytes },
		{ FIELD_DATA, &sample->data_kilobytes },
	};
	unsigned int found = 0U;
	char *line = NULL;
	size_t capacity = 0U;
	FILE *file = fopen(path, FILE_MODE_READ);

	if (file == NULL)
		return error_set(error, error_size, "could not open %s: %s", path, strerror(errno));
	/* Lines read "VmRSS:\t    1234 kB"; the values are always in kB. */
	while (getline(&line, &capacity, file) >= 0) {
		char name[FIELD_NAME_SIZE];
		uint64_t value;

		if (sscanf(line, "%31[^:]: %" SCNu64, name, &value) != 2)
			continue;
		for (size_t index = 0U; index < ARRAY_SIZE(fields); index++) {
			if (strcmp(name, fields[index].name) == 0) {
				*fields[index].value = value;
				found |= 1U << index;
			}
		}
	}
	free(line);
	(void)fclose(file);
	if (found != (1U << ARRAY_SIZE(fields)) - 1U)
		return error_set(error, error_size, "memory fields missing in %s", path);
	return 0;
}
