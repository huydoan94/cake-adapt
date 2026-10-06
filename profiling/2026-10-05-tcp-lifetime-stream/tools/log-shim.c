#include "logging/log.h"

#include <stdio.h>
#include <stdarg.h>

void log_message(enum log_level level, const char *format, ...)
{
	va_list arguments;
	static const char *const names[] = { "error", "warning", "notice", "info", "debug" };

	if ((unsigned int)level < sizeof(names) / sizeof(names[0]))
		(void)fprintf(stderr, "libbpf %s: ", names[level]);
	else
		(void)fprintf(stderr, "libbpf: ");
	va_start(arguments, format);
	(void)vfprintf(stderr, format, arguments);
	va_end(arguments);
}
