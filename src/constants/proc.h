#ifndef CONSTANTS_PROC_H_INCLUDED
#define CONSTANTS_PROC_H_INCLUDED

/* /proc files and fields, CPU and memory sampling. */

#include "constants/units.h"

#define PROC_STAT_PATH "/proc/stat"
#define CPU_PREFIX "cpu"
/* The name and user, nice, system and idle: what every kernel's /proc/stat has. */
#define CPU_MINIMUM_FIELDS 5
#define CPU_MAX_COUNT 65U
#define CPU_IDENTIFIER_SIZE 16U

/* /proc/self/status fields read for MEMORY records. */
#define PROC_SELF_STATUS_PATH "/proc/self/status"
#define PROC_FIELD_RSS "VmRSS"
#define PROC_FIELD_PEAK_RSS "VmHWM"
#define PROC_FIELD_ANONYMOUS "RssAnon"
#define PROC_FIELD_DATA "VmData"
/* Longer than any field name in /proc/<pid>/status; memory_read() scans at most 31. */
#define PROC_FIELD_NAME_SIZE 32U

/* MEMORY records: a week is about 60,000 lines, and a leak shows within minutes. */
#define MEMORY_SAMPLE_INTERVAL_US (10U * SECOND)

#endif
