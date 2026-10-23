#ifndef CONFIG_OPTIONS_H_INCLUDED
#define CONFIG_OPTIONS_H_INCLUDED

/*
 * Private to config/: the table that binds each UCI option to its member of
 * struct config, which loading, -L and -D walk in the same order.
 */

#include <stddef.h>
#include <stdint.h>

enum option_type {
	TYPE_BOOLEAN,
	TYPE_STRING,
	/* A non-negative decimal stored multiplied by scale, such as seconds in microseconds. */
	TYPE_SCALED,
};

struct option_binding {
	const char *name;
	enum option_type type;
	size_t offset;
	size_t size;
	uint64_t scale;
};

/* The binding at index, below config_option_count(). */
const struct option_binding *config_option(size_t index);

#endif
