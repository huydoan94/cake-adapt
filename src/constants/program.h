#ifndef CONSTANTS_PROGRAM_H_INCLUDED
#define CONSTANTS_PROGRAM_H_INCLUDED

/* Program identity, build features, the runtime directory and threads. */

#include "constants/units.h"

#define CLI_OPTIONS "C:DLfS:Vh"
#define PROGRAM_NAME "cake-adapt"
/* Set by the build from the package version; other builds report "unknown". */
#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "unknown"
#endif

/*
 * The package variant sets both: cake-adapt 0/0, cake-adapt-mqtt 1/0, cake-adapt-mqtt-ssl 1/1.
 * The default is cake-adapt's, as in src/Makefile.
 */
#ifndef MQTT_SUPPORTED
#define MQTT_SUPPORTED 0
#endif
#ifndef MQTT_TLS_SUPPORTED
#define MQTT_TLS_SUPPORTED 0
#endif

/* The init script's runtime directory; each running instance locks <section>.lock in it. */
#define RUNTIME_DIRECTORY "/var/run/" PROGRAM_NAME
#define LOCK_EXTENSION ".lock"
/* A pid in decimal, its newline and the terminator. */
#define LOCK_PID_TEXT_SIZE 24U

/* Bounded thread joins check this often. */
#define THREAD_JOIN_CHECK_US (10U * MILLISECOND)

#endif
