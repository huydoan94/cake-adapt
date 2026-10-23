#ifndef CONSTANTS_LOGGING_H_INCLUDED
#define CONSTANTS_LOGGING_H_INCLUDED

/* Record names, log files, the log writer and its buffers. */

#include "constants/units.h"

/* Record names. */
#define RECORD_DATA "DATA"
#define RECORD_CPU "CPU"
#define RECORD_CPU_RAW "CPU_RAW"
#define RECORD_DEBUG "DEBUG"
#define RECORD_ERROR "ERROR"
#define RECORD_INFO "INFO"
#define RECORD_LOAD "LOAD"
#define RECORD_MEMORY "MEMORY"
#define RECORD_REFLECTOR "REFLECTOR"
#define RECORD_SHAPER "SHAPER"
#define RECORD_SYSLOG "SYSLOG"
#define RECORD_SUMMARY "SUMMARY"
#define RECORD_TCP_QUEUE "TCP_QUEUE"
#define RECORD_TCP_INJECT "TCP_INJECT"
#define RECORD_WARNING "WARNING"

/* Reflector comparison columns, named as in cake-autorate's messages. */
#define REFLECTOR_SUM_OWD_BASELINES "sum_owd_baselines_us"
#define REFLECTOR_DL_OWD_DELTA_EWMA "dl_owd_delta_ewma_us"
#define REFLECTOR_UL_OWD_DELTA_EWMA "ul_owd_delta_ewma_us"

/* Log files, their names and exports. */
#define LOG_EXTENSION ".log"
/* Between the program name and a named instance's section in its log file name. */
#define LOG_SECTION_SEPARATOR "."
#define LOG_PREVIOUS_SUFFIX ".old"
#define GZIP_EXTENSION ".gz"
#define EXPORT_TIME_FORMAT "%Y_%m_%d_%H_%M_%S"
#define LOG_DATETIME_FORMAT "%Y-%m-%d-%H:%M:%S"
#define LOG_DATETIME_FALLBACK "1970-01-01-00:00:00"
#define FILE_MODE_READ "r"
#define FILE_MODE_WRITE "w"
#define FILE_MODE_APPEND_CLOEXEC "a+e"
#define GZIP_MODE_COMPRESSED "wb"
#define GZIP_MODE_TRANSPARENT "wbT"

/* Buffers. */
#define LOG_PATH_SIZE 512U
#define LOG_DATETIME_SIZE 20U
#define LOG_EXPORT_PATH_SIZE \
	(LOG_PATH_SIZE + LOG_DATETIME_SIZE + sizeof(LOG_EXTENSION) + sizeof(GZIP_EXTENSION))
#define LOG_MESSAGE_SIZE 2048U
#define LOG_COPY_BUFFER_SIZE 4096U
#define LOG_FILE_BUFFER_SIZE (16U * KILOBYTE)
/* UINT64_MAX seconds, a point, six digits and the terminating NUL. */
#define LOG_TIMESTAMP_SIZE 28U

/* The writer thread and its queue. */
/* Room for queued lines while the writer thread is slower than the records. */
#define LOG_QUEUE_BYTES (128U * KILOBYTE)
/* The queue fill at which a line wakes the writer instead of waiting for its timer. */
#define LOG_QUEUE_WAKE_BYTES (LOG_QUEUE_BYTES / 2U)
/* The writer's longest sleep with nothing queued, between maintenance checks. */
#define LOG_WRITER_IDLE_US SECOND
/* How long stopping waits for the writer to drain the queue. */
#define LOG_WRITER_STOP_US (2U * SECOND)

#endif
