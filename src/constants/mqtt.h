#ifndef CONSTANTS_MQTT_H_INCLUDED
#define CONSTANTS_MQTT_H_INCLUDED

/* MQTT forwarding: defaults, validation, queue and timing. */

#include "constants/units.h"

/* Defaults and validation. */
#define MQTT_DEFAULT_TOPIC "cake-adapt/log"
#define MQTT_DEFAULT_PORT 1883U
#define MQTT_INVALID_HOST_CHARACTERS " /[]\t\r\n"
#define MQTT_TOPIC_WILDCARDS "+#"

/* The sender: queue, timing and references. */
#define MQTT_QUEUE_RECORDS 32U
#define MQTT_LINE_SIZE 2048U
#define MQTT_RETRY_US (10U * SECOND)
#define MQTT_TIMEOUT_US (5U * SECOND)
/* The longest wait of the network loop and of the sender, between their checks. */
#define MQTT_WAIT_US (100U * MILLISECOND)
#define MQTT_STOP_US SECOND
/* Shorter than MQTT_STOP_US, so a sender abandoning a connection still stops in time. */
#define MQTT_NETWORK_STOP_US (500U * MILLISECOND)
#define MQTT_KEEPALIVE_SEC 30
/* The main thread's and the sender thread's references; each live network thread adds one. */
#define MQTT_SENDER_OWNERS 2U

#endif
