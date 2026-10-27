#define _GNU_SOURCE

#include "daemon/state.h"

#include "common/arith.h"
#include "common/convert.h"
#include "logging/log.h"
#include "logging/records.h"
#include "platform/memory.h"
#include "constants/proc.h"
#include "constants/text.h"
#include "constants/units.h"

#include <libubox/utils.h>

#include <errno.h>
#include <signal.h>
#include <string.h>

/*
 * Reads the counters and reports failure, recovery and a changed CPU count.
 * cpu_read() initializes only the counters the kernel returns.
 */
static bool read_cpu(struct daemon *daemon, struct cpu_sample *sample)
{
	struct daemon_upkeep *upkeep = &daemon->upkeep;
	char error[ERROR_SIZE] = { 0 };

	if (cpu_read(PROC_STAT_PATH, sample, error, sizeof(error)) != 0) {
		daemon_degraded(
			&upkeep->cpu_observation_failed,
			"CPU observation degraded: %s",
			error
		);
		return false;
	}
	daemon_recovered(&upkeep->cpu_observation_failed, "CPU observation recovered");
	if (upkeep->cpu_count != sample->count) {
		upkeep->cpu_count = sample->count;
		cpu_init(&upkeep->cpu_monitor);
		log_message(LOG_LEVEL_DEBUG, "Detected %zu CPU cores.", sample->count - 1U);
		log_print_cpu_headers(
			sample,
			daemon->config->output_cpu_stats,
			daemon->config->output_cpu_raw_stats
		);
	}
	return true;
}

static void handle_cpu_timer(struct uloop_interval *timer)
{
	struct daemon *daemon = __extension__ container_of(timer, struct daemon, upkeep.cpu_timer);
	struct daemon_upkeep *upkeep = &daemon->upkeep;
	const struct controller_activity *activity = &daemon->activity;
	struct cpu_sample sample;
	struct cpu_busy usage[CPU_MAX_COUNT];

	if (activity->state != CONTROLLER_ACTIVITY_RUNNING || !read_cpu(daemon, &sample))
		return;
	if (daemon->config->output_cpu_raw_stats)
		log_cpu_raw(&sample);
	if (daemon->config->output_cpu_stats) {
		cpu_usage(&upkeep->cpu_monitor, &sample, usage);
		log_cpu(&sample, usage);
	}
}

static void handle_log_timer(struct uloop_interval *timer)
{
	(void)timer;
	log_tick();
}

static void handle_log_export_signal(struct uloop_signal *signal)
{
	(void)signal;
	log_message(LOG_LEVEL_DEBUG, "received log file export signal so exporting log file.");
	log_request_export();
}

static void handle_log_reset_signal(struct uloop_signal *signal)
{
	(void)signal;
	log_message(
		LOG_LEVEL_DEBUG,
		"received log file reset signal so flushing log and resetting log file."
	);
	log_request_reset();
}

static void watch_cpu(struct daemon *daemon)
{
	const struct config *config = daemon->config;
	struct daemon_upkeep *upkeep = &daemon->upkeep;
	struct cpu_sample sample;

	if (!config_cpu_stats(config))
		return;
	/* Detect the CPUs and print the headers now; records start with the timer. */
	(void)read_cpu(daemon, &sample);
	if (uloop_interval_set(
		    &upkeep->cpu_timer,
		    us_to_ms(config->monitor_cpu_usage_interval_us)
	    ) != 0) {
		log_message(LOG_LEVEL_WARNING, "could not monitor CPU timer: %s", strerror(errno));
	}
}

/* Sampled in every activity state: memory matters while idle too. */
static void handle_memory_timer(struct uloop_interval *timer)
{
	struct daemon_upkeep *upkeep =
		__extension__ container_of(timer, struct daemon_upkeep, memory_timer);
	char error[ERROR_SIZE] = { 0 };
	struct memory_sample sample;

	if (memory_read(PROC_SELF_STATUS_PATH, &sample, error, sizeof(error)) != 0) {
		daemon_degraded(
			&upkeep->memory_observation_failed,
			"memory observation degraded: %s",
			error
		);
		return;
	}
	daemon_recovered(&upkeep->memory_observation_failed, "memory observation recovered");
	memory_track_peak(&sample, &upkeep->memory_peak_bytes);
	log_memory(&sample);
}

static void watch_memory(struct daemon *daemon)
{
	struct daemon_upkeep *upkeep = &daemon->upkeep;

	if (!daemon->config->output_memory_stats)
		return;
	/* The first record at start, then one per interval. */
	handle_memory_timer(&upkeep->memory_timer);
	if (uloop_interval_set(&upkeep->memory_timer, us_to_ms(MEMORY_SAMPLE_INTERVAL_US)) != 0) {
		log_message(
			LOG_LEVEL_WARNING,
			"could not monitor memory timer: %s",
			strerror(errno)
		);
	}
}

static void watch_log_maintenance(struct daemon *daemon)
{
	const struct config *config = daemon->config;
	struct daemon_upkeep *upkeep = &daemon->upkeep;
	uint64_t log_timer_us = config->log_file_buffer_timeout_us;

	if (!config->log_to_file)
		return;
	/* Without a buffer timer, still wake up to rotate just after the maximum age. */
	if (log_timer_us == 0U && config->log_file_max_time_us > 0U)
		log_timer_us = saturating_add(config->log_file_max_time_us, MILLISECOND);
	if (log_timer_us > 0U &&
	    uloop_interval_set(&upkeep->log_timer, us_to_ms(log_timer_us)) != 0) {
		log_message(LOG_LEVEL_WARNING, "log timer degraded: %s", strerror(errno));
	}
	if (uloop_signal_add(&upkeep->log_export_signal) != 0)
		log_message(LOG_LEVEL_WARNING, "log export signal degraded: %s", strerror(errno));
	if (uloop_signal_add(&upkeep->log_reset_signal) != 0)
		log_message(LOG_LEVEL_WARNING, "log reset signal degraded: %s", strerror(errno));
}

void upkeep_init(struct daemon *daemon)
{
	struct daemon_upkeep *upkeep = &daemon->upkeep;

	upkeep->cpu_timer = (struct uloop_interval){ .cb = handle_cpu_timer };
	upkeep->memory_timer = (struct uloop_interval){ .cb = handle_memory_timer };
	upkeep->log_timer = (struct uloop_interval){ .cb = handle_log_timer };
	upkeep->log_export_signal =
		(struct uloop_signal){ .cb = handle_log_export_signal, .signo = SIGUSR1 };
	upkeep->log_reset_signal =
		(struct uloop_signal){ .cb = handle_log_reset_signal, .signo = SIGUSR2 };
}

void upkeep_start(struct daemon *daemon)
{
	watch_cpu(daemon);
	watch_memory(daemon);
	watch_log_maintenance(daemon);
}

void upkeep_stop(struct daemon *daemon)
{
	struct daemon_upkeep *upkeep = &daemon->upkeep;

	(void)uloop_interval_cancel(&upkeep->cpu_timer);
	(void)uloop_interval_cancel(&upkeep->memory_timer);
	(void)uloop_interval_cancel(&upkeep->log_timer);
	(void)uloop_signal_delete(&upkeep->log_export_signal);
	(void)uloop_signal_delete(&upkeep->log_reset_signal);
}
