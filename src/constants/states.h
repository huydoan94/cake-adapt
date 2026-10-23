#ifndef CONSTANTS_STATES_H_INCLUDED
#define CONSTANTS_STATES_H_INCLUDED

/* Runtime states, directions, timers and status words shared by records and messages. */

#define STATE_BELOW_CAPACITY "below-capacity"
#define STATE_ACK_SHARE "ack-share"
#define STATE_CLEAR "clear"
#define STATE_CONGESTION "congestion"
#define STATE_DETECTED "detected"
#define STATE_HIGH "high"
#define STATE_HIGH_LOAD "high-load"
#define STATE_IDLE "idle"
#define STATE_IDLE_UPPER "IDLE"
#define STATE_INITIAL "initial"
#define STATE_INITIALIZED "initialized"
#define STATE_LOW "low"
#define STATE_RECONCILE "reconcile"
#define STATE_RECOVERED "recovered"
#define STATE_RETURN_TO_BASE "return-to-base"
#define STATE_RUNNING "running"
#define STATE_STALL "stall"
#define STATE_RUNNING_UPPER "RUNNING"
#define STATE_SATURATED "saturated"
#define STATE_STALL_UPPER "STALL"
#define STATE_UNCHANGED "unchanged"
#define STATE_UNKNOWN "unknown"
#define STATE_UNLIMITED "unlimited"
#define STATE_UNAVAILABLE "unavailable"
#define STATE_VALID "valid"

#define STATUS_DISABLED "disabled"
#define STATUS_ENABLED "enabled"

#define DIRECTION_DOWNLOAD "download"
#define DIRECTION_DOWNLOAD_SHORT "dl"
#define DIRECTION_UPLOAD "upload"
#define DIRECTION_UPLOAD_SHORT "ul"
#define BUFFERBLOAT_SUFFIX "_bb"

/* Timer names, as messages name them. */
#define TIMER_REFLECTOR_HEALTH "reflector health"
#define TIMER_TRAFFIC "traffic"

#define READBACK_MISMATCH "readback did not match"

#endif
