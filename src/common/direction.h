#ifndef COMMON_DIRECTION_H_INCLUDED
#define COMMON_DIRECTION_H_INCLUDED

/*
 * The two shaped directions. Everything kept per direction is an array
 * indexed by this enum, so code that treats both alike is one loop.
 */

#include "constants/states.h"

enum direction {
	DIRECTION_DOWNLOAD,
	DIRECTION_UPLOAD,
	DIRECTION_COUNT
};

/* "download" or "upload", as messages name a direction. */
static inline const char *direction_name(enum direction direction)
{
	return direction == DIRECTION_DOWNLOAD ? DIRECTION_NAME_DOWNLOAD : DIRECTION_NAME_UPLOAD;
}

/* "dl" or "ul", as cake-autorate's records and options name a direction. */
static inline const char *direction_short_name(enum direction direction)
{
	return direction == DIRECTION_DOWNLOAD ? DIRECTION_SHORT_NAME_DOWNLOAD :
						 DIRECTION_SHORT_NAME_UPLOAD;
}

#endif
