#include "common/error.h"

#include <stdarg.h>
#include <stdio.h>

int error_set(char *error, size_t error_size, const char *format, ...)
{
	va_list arguments;

	if (error == NULL || error_size == 0U)
		return -1;

	va_start(arguments, format);
	(void)vsnprintf(error, error_size, format, arguments);
	va_end(arguments);
	return -1;
}
