#ifndef DAEMON_DAEMON_H_INCLUDED
#define DAEMON_DAEMON_H_INCLUDED

#include "config/config.h"

int daemon_run(const struct config *config, const char *section_name);

#endif
