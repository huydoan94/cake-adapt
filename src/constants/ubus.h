#ifndef CONSTANTS_UBUS_H_INCLUDED
#define CONSTANTS_UBUS_H_INCLUDED

/* The ubus status object, its fields and its thread. */

#include "constants/options.h"
#include "constants/program.h"
#include "constants/units.h"

/* ubus: the status object is UBUS_OBJECT_PREFIX "." <section>, method UBUS_METHOD_STATUS. */
#define UBUS_OBJECT_PREFIX PROGRAM_NAME
#define UBUS_METHOD_STATUS "status"
#define PUBLISH_OBJECT_NAME_SIZE (sizeof(UBUS_OBJECT_PREFIX) + 1U + CONFIG_STRING_SIZE)

/* Status reply fields. */
#define STATUS_FIELD_VERSION "version"
#define STATUS_FIELD_PID "pid"
#define STATUS_FIELD_STARTED "started"
#define STATUS_FIELD_STATE "state"
#define STATUS_FIELD_DOWNLOAD "download"
#define STATUS_FIELD_UPLOAD "upload"
#define STATUS_FIELD_INTERFACE "interface"
#define STATUS_FIELD_AVAILABLE "available"
#define STATUS_FIELD_SHAPER "shaper_bps"
#define STATUS_FIELD_ACHIEVED "achieved_bps"
#define STATUS_FIELD_LOAD "load_e6"
#define STATUS_FIELD_DELAY "delay_us"
#define STATUS_FIELD_BUFFERBLOAT "bufferbloat"
#define STATUS_FIELD_TCP_QUEUE "tcp_queue_us"

/* The status thread. */
/* The thread's longest wait, both between connection attempts and for requests. */
#define PUBLISH_WAIT_US SECOND
/* How long shutdown waits for the thread before leaving it behind, checked this often. */
#define PUBLISH_STOP_US (2U * SECOND)

#endif
