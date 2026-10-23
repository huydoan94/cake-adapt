#define _POSIX_C_SOURCE 200809L

#include "platform/memory.h"

#include "common/error.h"
#include "common/utils.h"
#include "constants/logging.h"
#include "constants/proc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int memory_read(const char *path, struct memory_sample *sample, char *error, size_t error_size)
{
	const struct {
		const char *name;
		uint64_t *value;
	} fields[] = {
		{ PROC_FIELD_RSS, &sample->rss_bytes },
		{ PROC_FIELD_PEAK_RSS, &sample->peak_rss_bytes },
		{ PROC_FIELD_ANONYMOUS, &sample->anonymous_bytes },
		{ PROC_FIELD_DATA, &sample->data_bytes },
	};
	unsigned int found = 0U;
	char *line = NULL;
	size_t capacity = 0U;
	FILE *file = fopen(path, FILE_MODE_READ);

	if (file == NULL)
		return error_set(error, error_size, "could not open %s: %s", path, strerror(errno));
	/* Lines read "VmRSS:\t    1234 kB"; the values are always in kB. */
	while (getline(&line, &capacity, file) >= 0) {
		char name[PROC_FIELD_NAME_SIZE];
		uint64_t kilobytes;

		if (sscanf(line, "%31[^:]: %" SCNu64, name, &kilobytes) != 2)
			continue;
		for (size_t index = 0U; index < ARRAY_SIZE(fields); index++) {
			if (strcmp(name, fields[index].name) == 0) {
				*fields[index].value = kbyte_to_byte(kilobytes);
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

void memory_track_peak(struct memory_sample *sample, uint64_t *peak_bytes)
{
	*peak_bytes = max_u64(*peak_bytes, max_u64(sample->peak_rss_bytes, sample->rss_bytes));
	sample->peak_rss_bytes = *peak_bytes;
}
