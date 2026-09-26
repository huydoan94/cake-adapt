#ifndef CONSTANTS_H_INCLUDED
#define CONSTANTS_H_INCLUDED

#include <stdint.h>

/* Unit conversions and fixed-point scales shared across modules. */
#define THOUSAND UINT64_C(1000)
#define MILLION (THOUSAND * THOUSAND)
#define KILOBIT THOUSAND
#define MEGABIT (THOUSAND * KILOBIT)
#define MICROSECOND UINT64_C(1)
#define MILLISECOND (THOUSAND * MICROSECOND)
#define SECOND (THOUSAND * MILLISECOND)
#define MINUTE (UINT64_C(60) * SECOND)
#define KIBIBYTE UINT64_C(1024)
#define PERCENT UINT64_C(100)
#define BITS_PER_BYTE UINT64_C(8)
#define FACTOR_PER_PERCENT (MILLION / PERCENT)
#define MILLISECONDS_PER_SECOND (SECOND / MILLISECOND)
#define MICROSECONDS_PER_MILLISECOND (MILLISECOND / MICROSECOND)
#define MICROSECONDS_PER_SECOND SECOND
#define MICROSECONDS_PER_MINUTE MINUTE
#define NANOSECONDS_PER_MICROSECOND THOUSAND
#define NANOSECONDS_PER_MILLISECOND \
    (THOUSAND * NANOSECONDS_PER_MICROSECOND)
#define NANOSECONDS_PER_SECOND \
    ((long)(THOUSAND * NANOSECONDS_PER_MILLISECOND))

/* Program identity and integration names. */
#define CLI_OPTIONS "C:LfS:Vh"
#define IFB_PREFIX "ifb4"
#define PINGER_METHOD_FPING "fping"
#define PROGRAM_NAME "cake-adapt"
#define QDISC_KIND "cake"
#define UCI_PACKAGE PROGRAM_NAME
#define UCI_SECTION_TYPE "cake_adapt"

/* Runtime states, directions, timers, and generic text tokens. */
#define STATE_BELOW_CAPACITY "below-capacity"
#define STATE_CLEAR "clear"
#define STATE_CONGESTION "congestion"
#define STATE_DETECTED "detected"
#define STATE_HIGH "high"
#define STATE_HIGH_LOAD "high-load"
#define STATE_IDLE "idle"
#define STATE_IDLE_UPPER "IDLE"
#define STATE_INITIAL "initial"
#define STATE_INVALID "invalid"
#define STATE_LOW "low"
#define STATE_RECONCILE "reconcile"
#define STATE_RETURN_TO_BASE "return-to-base"
#define STATE_RUNNING_UPPER "RUNNING"
#define STATE_SATURATED "saturated"
#define STATE_STALL_UPPER "STALL"
#define STATE_UNCHANGED "unchanged"
#define STATE_UNKNOWN "unknown"
#define STATUS_DISABLED "disabled"
#define STATUS_ENABLED "enabled"

#define BUFFERBLOAT_SUFFIX "_bb"
#define DIRECTION_DOWNLOAD "download"
#define DIRECTION_DOWNLOAD_SHORT "dl"
#define DIRECTION_UPLOAD "upload"
#define DIRECTION_UPLOAD_SHORT "ul"
#define READBACK_MISMATCH "readback did not match"
#define TIMER_REFLECTOR_HEALTH "reflector health"
#define TIMER_TRAFFIC "traffic"

#define DECIMAL_DIGITS "0123456789"
#define TARGET_CHARACTERS \
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.:_-"
#define FRACTION_ZEROES "000000"
#define EMPTY_STRING ""
#define NULL_VALUE "(null)"

/* fping command-line arguments and output grammar. */
#define FPING_BYTES_SEPARATOR " bytes, "
#define FPING_FIELD_SEPARATOR ", "
#define FPING_INTERFACE_LONG "--iface"
#define FPING_INTERFACE_LONG_PREFIX "--iface="
#define FPING_INTERFACE_SHORT "-I"
#define FPING_INTERVAL "--interval"
#define FPING_LOOP "--loop"
#define FPING_MILLISECONDS_SUFFIX " ms"
#define FPING_PATH "/usr/bin/fping"
#define FPING_PERIOD "--period"
#define FPING_SEQUENCE_SEPARATOR " : ["
#define FPING_TIMEOUT "--timeout"
#define FPING_TIMEOUT_SUFFIX ", timed out"
#define FPING_TIMESTAMP "--timestamp"
#define NULL_DEVICE_PATH "/dev/null"

/* Proc, file, archive, and log-format constants. */
#define CPU_PREFIX "cpu"
#define PROC_STAT_PATH "/proc/stat"
#define FILE_MODE_READ "r"
#define FILE_MODE_WRITE "w"
#define FILE_MODE_APPEND_CLOEXEC "a+e"
#define GZIP_MODE_COMPRESSED "wb"
#define GZIP_MODE_TRANSPARENT "wbT"
#define LOG_EXTENSION ".log"
#define GZIP_EXTENSION ".gz"
#define LOG_PREVIOUS_SUFFIX ".old"
#define EXPORT_TIME_FORMAT "%Y_%m_%d_%H_%M_%S"
#define LOG_DATETIME_FORMAT "%Y-%m-%d-%H:%M:%S"
#define LOG_DATETIME_FALLBACK "1970-01-01-00:00:00"

/* Structured record names. */
#define RECORD_DATA "DATA"
#define RECORD_CPU "CPU"
#define RECORD_CPU_RAW "CPU_RAW"
#define RECORD_DEBUG "DEBUG"
#define RECORD_ERROR "ERROR"
#define RECORD_INFO "INFO"
#define RECORD_LOAD "LOAD"
#define RECORD_REFLECTOR "REFLECTOR"
#define RECORD_SHAPER "SHAPER"
#define RECORD_SYSLOG "SYSLOG"
#define RECORD_SUMMARY "SUMMARY"
#define RECORD_WARNING "WARNING"

/* UCI option names, grouped in the same order as the configuration model. */
#define OPTION_ENABLED "enabled"
#define OPTION_ADJUST_DOWNLOAD "adjust_dl_shaper_rate"
#define OPTION_ADJUST_UPLOAD "adjust_ul_shaper_rate"
#define OPTION_OUTPUT_PROCESSING_STATS "output_processing_stats"
#define OPTION_OUTPUT_LOAD_STATS "output_load_stats"
#define OPTION_OUTPUT_REFLECTOR_STATS "output_reflector_stats"
#define OPTION_OUTPUT_SUMMARY_STATS "output_summary_stats"
#define OPTION_OUTPUT_CAKE_CHANGES "output_cake_changes"
#define OPTION_OUTPUT_CPU_STATS "output_cpu_stats"
#define OPTION_OUTPUT_CPU_RAW_STATS "output_cpu_raw_stats"
#define OPTION_DEBUG "debug"
#define OPTION_LOG_DEBUG_TO_SYSLOG "log_DEBUG_messages_to_syslog"
#define OPTION_LOG_TO_FILE "log_to_file"
#define OPTION_RANDOMIZE_REFLECTORS "randomize_reflectors"
#define OPTION_RETAIN_REFLECTOR_STATS "retain_reflector_stats"
#define OPTION_ENABLE_SLEEP_FUNCTION "enable_sleep_function"
#define OPTION_MIN_SHAPER_RATES_ENFORCEMENT "min_shaper_rates_enforcement"
#define OPTION_LOG_FILE_EXPORT_COMPRESS "log_file_export_compress"
#define OPTION_INTERFACE "interface"
#define OPTION_UPLOAD_INTERFACE "ul_if"
#define OPTION_DOWNLOAD_INTERFACE "dl_if"
#define OPTION_CONFIG_FILE "config_file"
#define OPTION_LOG_FILE_PATH_OVERRIDE "log_file_path_override"
#define OPTION_PINGER_METHOD "pinger_method"
#define OPTION_PING_EXTRA_ARGS "ping_extra_args"
#define OPTION_PING_PREFIX_STRING "ping_prefix_string"
#define OPTION_LOG_FILE_MAX_TIME "log_file_max_time_mins"
#define OPTION_LOG_FILE_MAX_SIZE "log_file_max_size_KB"
#define OPTION_NO_PINGERS "no_pingers"
#define OPTION_REFLECTOR_PING_INTERVAL "reflector_ping_interval_s"
#define OPTION_MIN_DOWNLOAD_RATE "min_dl_shaper_rate_kbps"
#define OPTION_BASE_DOWNLOAD_RATE "base_dl_shaper_rate_kbps"
#define OPTION_MAX_DOWNLOAD_RATE "max_dl_shaper_rate_kbps"
#define OPTION_MIN_UPLOAD_RATE "min_ul_shaper_rate_kbps"
#define OPTION_BASE_UPLOAD_RATE "base_ul_shaper_rate_kbps"
#define OPTION_MAX_UPLOAD_RATE "max_ul_shaper_rate_kbps"
#define OPTION_CONNECTION_ACTIVE_THRESHOLD "connection_active_thr_kbps"
#define OPTION_CONNECTION_STALL_THRESHOLD "connection_stall_thr_kbps"
#define OPTION_DOWNLOAD_AVG_ADJUST_UP "dl_avg_owd_delta_max_adjust_up_thr_ms"
#define OPTION_UPLOAD_AVG_ADJUST_UP "ul_avg_owd_delta_max_adjust_up_thr_ms"
#define OPTION_DOWNLOAD_DELAY_THRESHOLD "dl_owd_delta_delay_thr_ms"
#define OPTION_UPLOAD_DELAY_THRESHOLD "ul_owd_delta_delay_thr_ms"
#define OPTION_DOWNLOAD_AVG_ADJUST_DOWN "dl_avg_owd_delta_max_adjust_down_thr_ms"
#define OPTION_UPLOAD_AVG_ADJUST_DOWN "ul_avg_owd_delta_max_adjust_down_thr_ms"
#define OPTION_SUSTAINED_IDLE_SLEEP "sustained_idle_sleep_thr_s"
#define OPTION_LOG_FILE_BUFFER_TIMEOUT "log_file_buffer_timeout_ms"
#define OPTION_IRTT_SESSION_DURATION "irtt_session_duration_m"
#define OPTION_TRAFFIC_MONITOR_INTERVAL "monitor_achieved_rates_interval_ms"
#define OPTION_CPU_MONITOR_INTERVAL "monitor_cpu_usage_interval_ms"
#define OPTION_BUFFERBLOAT_WINDOW "bufferbloat_detection_window"
#define OPTION_BUFFERBLOAT_THRESHOLD "bufferbloat_detection_thr"
#define OPTION_ALPHA_BASELINE_INCREASE "alpha_baseline_increase"
#define OPTION_ALPHA_BASELINE_DECREASE "alpha_baseline_decrease"
#define OPTION_ALPHA_DELTA_EWMA "alpha_delta_ewma"
#define OPTION_RATE_MIN_DOWN_BUFFERBLOAT "shaper_rate_min_adjust_down_bufferbloat"
#define OPTION_RATE_MAX_DOWN_BUFFERBLOAT "shaper_rate_max_adjust_down_bufferbloat"
#define OPTION_RATE_MIN_UP_HIGH_LOAD "shaper_rate_min_adjust_up_load_high"
#define OPTION_RATE_MAX_UP_HIGH_LOAD "shaper_rate_max_adjust_up_load_high"
#define OPTION_RATE_DOWN_LOW_LOAD "shaper_rate_adjust_down_load_low"
#define OPTION_RATE_UP_LOW_LOAD "shaper_rate_adjust_up_load_low"
#define OPTION_HIGH_LOAD_THRESHOLD "high_load_thr"
#define OPTION_BUFFERBLOAT_REFRACTORY "bufferbloat_refractory_period_ms"
#define OPTION_DECAY_REFRACTORY "decay_refractory_period_ms"
#define OPTION_REFLECTOR_HEALTH_INTERVAL "reflector_health_check_interval_s"
#define OPTION_REFLECTOR_RESPONSE_DEADLINE "reflector_response_deadline_s"
#define OPTION_REFLECTOR_MISBEHAVING_WINDOW \
    "reflector_misbehaving_detection_window"
#define OPTION_REFLECTOR_MISBEHAVING_THRESHOLD \
    "reflector_misbehaving_detection_thr"
#define OPTION_REFLECTOR_REPLACEMENT_INTERVAL \
    "reflector_replacement_interval_mins"
#define OPTION_REFLECTOR_COMPARISON_INTERVAL \
    "reflector_comparison_interval_mins"
#define OPTION_REFLECTOR_BASELINE_DELTA \
    "reflector_sum_owd_baselines_delta_thr_ms"
#define OPTION_REFLECTOR_EWMA_DELTA \
    "reflector_owd_delta_ewma_delta_thr_ms"
#define OPTION_STALL_DETECTION_THRESHOLD "stall_detection_thr"
#define OPTION_GLOBAL_PING_TIMEOUT "global_ping_response_timeout_s"
#define OPTION_INTERFACE_UP_INTERVAL "if_up_check_interval_s"
#define OPTION_REFLECTORS "reflectors"
#define OPTION_LATENCY_TARGET "latency_target"

/* Accepted textual boolean values. */
#define BOOLEAN_TRUE_NUMERIC "1"
#define BOOLEAN_TRUE "true"
#define BOOLEAN_YES "yes"
#define BOOLEAN_ON "on"
#define BOOLEAN_FALSE_NUMERIC "0"
#define BOOLEAN_FALSE "false"
#define BOOLEAN_NO "no"
#define BOOLEAN_OFF "off"

#endif
