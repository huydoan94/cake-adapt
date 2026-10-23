#define _POSIX_C_SOURCE 200809L

#include "logging/output.h"
#include "common/error.h"
#include "common/helpers.h"
#include "common/utils.h"
#include "constants/logging.h"

#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

struct entry_header {
	uint8_t kind;
	uint8_t priority;
	uint16_t bytes;
};

/*
 * The writer thread (log_start_writer): every write to the log file, stdout
 * and syslog, rotation, export and reset happen there, so slow storage or a
 * stuck logd never holds up the main loop. The main thread only queues, into
 * a bounded FIFO: when it is full, the oldest entries make room for the new
 * one and are counted, so the newest records are always kept. The writer
 * reports the count where they went missing, before the oldest entry left.
 * An overwritten export or reset request is still carried out.
 */
struct log_writer {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	unsigned char queue[LOG_QUEUE_BYTES];
	size_t first;
	size_t used;
	/* Overwritten since the writer last took an entry, all older than first. */
	uint64_t overwritten_lines;
	uint64_t overwritten_syslog;
	bool export_pending;
	bool reset_pending;
	bool stop;
};

static struct log_writer *writer;

bool writer_running(void)
{
	return writer != NULL;
}

/* Appends one entry to the queue, which has room for it; the lock is held. */
static void
put_entry(struct log_writer *state, enum entry_kind kind, int priority, const char *text)
{
	size_t length = strnlen(text, LOG_MESSAGE_SIZE - 1U) + 1U;
	struct entry_header header = {
		.kind = (uint8_t)kind,
		.priority = (uint8_t)priority,
		.bytes = (uint16_t)length,
	};
	const unsigned char *parts[] = { (const unsigned char *)&header,
					 (const unsigned char *)text };
	const size_t sizes[] = { sizeof(header), length };
	size_t end = (state->first + state->used) % LOG_QUEUE_BYTES;

	for (size_t part = 0U; part < ARRAY_SIZE(parts); part++) {
		size_t room = LOG_QUEUE_BYTES - end;
		size_t head = sizes[part] < room ? sizes[part] : room;

		memcpy(state->queue + end, parts[part], head);
		memcpy(state->queue, parts[part] + head, sizes[part] - head);
		end = (end + sizes[part]) % LOG_QUEUE_BYTES;
	}
	/* The text is stored NUL-terminated, even when strnlen() cut it. */
	state->queue[(end + LOG_QUEUE_BYTES - 1U) % LOG_QUEUE_BYTES] = '\0';
	state->used += sizeof(header) + length;
}

static size_t entry_bytes(const char *text)
{
	return sizeof(struct entry_header) + strnlen(text, LOG_MESSAGE_SIZE - 1U) + 1U;
}

/* Reads the oldest entry's header; the lock is held. */
static struct entry_header oldest_header(const struct log_writer *state)
{
	struct entry_header header;
	unsigned char *bytes = (unsigned char *)&header;
	size_t head = LOG_QUEUE_BYTES - state->first;

	if (head > sizeof(header))
		head = sizeof(header);
	memcpy(bytes, state->queue + state->first, head);
	memcpy(bytes + head, state->queue, sizeof(header) - head);
	return header;
}

/*
 * Removes the oldest entry to make room, the lock held: a line or syslog entry
 * is counted as overwritten, an export or reset request is kept as pending.
 */
static void overwrite_oldest(struct log_writer *state)
{
	struct entry_header header = oldest_header(state);
	size_t bytes = sizeof(header) + header.bytes;

	if (header.kind == ENTRY_EXPORT)
		state->export_pending = true;
	else if (header.kind == ENTRY_RESET)
		state->reset_pending = true;
	else if (header.kind == ENTRY_SYSLOG)
		state->overwritten_syslog++;
	else
		state->overwritten_lines++;
	state->first = (state->first + bytes) % LOG_QUEUE_BYTES;
	state->used -= bytes;
}

/*
 * Whether a new entry wakes the writer; the lock is held. Syslog entries,
 * requests, lines written out at once (no buffer timeout, or a terminal) and a
 * half-full queue do. Other lines wait for the writer's timer, at most the
 * buffer timeout, as they waited in the file buffer before: a wake per record
 * cost a thread switch per record.
 */
static bool entry_wakes_writer(const struct log_writer *state, enum entry_kind kind)
{
	return kind != ENTRY_LINE || file_buffer_timeout_us() == 0U || log_stdout_enabled() ||
	       state->used >= LOG_QUEUE_WAKE_BYTES;
}

void writer_queue(enum entry_kind kind, int priority, const char *text)
{
	struct log_writer *state = writer;
	size_t bytes = entry_bytes(text);

	(void)pthread_mutex_lock(&state->lock);
	while (LOG_QUEUE_BYTES - state->used < bytes)
		overwrite_oldest(state);
	put_entry(state, kind, priority, text);
	if (entry_wakes_writer(state, kind))
		(void)pthread_cond_signal(&state->wake);
	(void)pthread_mutex_unlock(&state->lock);
}

/* Copies the oldest entry out of the queue, its text into text; the lock is held. */
static struct entry_header take_entry(struct log_writer *state, char text[LOG_MESSAGE_SIZE])
{
	struct entry_header header;
	unsigned char *parts[] = { (unsigned char *)&header, (unsigned char *)text };
	size_t sizes[] = { sizeof(header), 0U };

	for (size_t part = 0U; part < ARRAY_SIZE(parts); part++) {
		size_t head;

		if (part == 1U)
			sizes[part] = header.bytes;
		head = LOG_QUEUE_BYTES - state->first;
		if (sizes[part] < head)
			head = sizes[part];
		memcpy(parts[part], state->queue + state->first, head);
		memcpy(parts[part] + head, state->queue, sizes[part] - head);
		state->first = (state->first + sizes[part]) % LOG_QUEUE_BYTES;
		state->used -= sizes[part];
	}
	return header;
}

static void write_entry(struct entry_header header, const char *text)
{
	switch ((enum entry_kind)header.kind) {
	case ENTRY_LINE:
		file_sink_line(text);
		break;
	case ENTRY_SYSLOG:
		syslog(header.priority, "%s", text);
		break;
	case ENTRY_EXPORT:
		log_export_now();
		break;
	case ENTRY_RESET:
		log_reset_now();
		break;
	}
}

/* Waits, the lock held, until an entry is queued, a stop is asked or the idle time ends. */
static void wait_for_entry(struct log_writer *state)
{
	uint64_t buffer_timeout_us = file_buffer_timeout_us();
	uint64_t idle_us = buffer_timeout_us > 0U ? min_u64(buffer_timeout_us, LOG_WRITER_IDLE_US) :
						    LOG_WRITER_IDLE_US;
	struct timespec deadline =
		us_to_timespec(saturating_add(log_clock_us(CLOCK_MONOTONIC), idle_us));

	(void)pthread_cond_timedwait(&state->wake, &state->lock, &deadline);
}

static void *writer_thread(void *argument)
{
	struct log_writer *state = argument;
	char text[LOG_MESSAGE_SIZE];

	(void)pthread_mutex_lock(&state->lock);
	for (;;) {
		struct entry_header header;
		uint64_t overwritten_lines;
		uint64_t overwritten_syslog;
		bool export_pending;
		bool reset_pending;
		bool taken;

		if (state->used == 0U && state->stop)
			break;
		if (state->used == 0U)
			wait_for_entry(state);
		/* What was overwritten is older than the entry taken now: report it first. */
		overwritten_lines = state->overwritten_lines;
		overwritten_syslog = state->overwritten_syslog;
		export_pending = state->export_pending;
		reset_pending = state->reset_pending;
		state->overwritten_lines = 0U;
		state->overwritten_syslog = 0U;
		state->export_pending = false;
		state->reset_pending = false;
		taken = state->used > 0U;
		if (taken)
			header = take_entry(state, text);
		(void)pthread_mutex_unlock(&state->lock);
		/*
		 * Overwritten requests run before the note, so a reset cannot erase
		 * it. Repeated or interleaved requests collapse into one export, then
		 * one reset: the export loses nothing either way.
		 */
		if (export_pending)
			log_export_now();
		if (reset_pending)
			log_reset_now();
		if (overwritten_lines > 0U || overwritten_syslog > 0U)
			log_note(
				LOG_LEVEL_WARNING,
				"%" PRIu64 " older log lines and %" PRIu64
				" syslog messages were overwritten: the log writer fell behind",
				overwritten_lines,
				overwritten_syslog
			);
		if (taken)
			write_entry(header, text);
		file_tick();
		(void)pthread_mutex_lock(&state->lock);
	}
	(void)pthread_mutex_unlock(&state->lock);
	file_flush();
	return NULL;
}

int log_start_writer(char *error, size_t error_size)
{
	struct log_writer *state;
	pthread_condattr_t attributes;
	sigset_t all;
	sigset_t previous;
	int result;

	if (writer != NULL)
		return 0;
	state = calloc(1U, sizeof(*state));
	if (state == NULL)
		return error_set(error, error_size, "could not allocate the log writer");
	if (pthread_mutex_init(&state->lock, NULL) != 0) {
		(void)error_set(error, error_size, "could not prepare the log writer");
		goto free_state;
	}
	if (pthread_condattr_init(&attributes) != 0) {
		(void)error_set(error, error_size, "could not prepare the log writer");
		goto destroy_lock;
	}
	/* Waits are timed on the monotonic clock, as the rest of the daemon. */
	(void)pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	result = pthread_cond_init(&state->wake, &attributes);
	(void)pthread_condattr_destroy(&attributes);
	if (result != 0) {
		(void)error_set(error, error_size, "could not prepare the log writer");
		goto destroy_lock;
	}
	/* Signals stay with the main thread's event loop. */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	result = pthread_create(&state->thread, NULL, writer_thread, state);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	if (result != 0) {
		(void)error_set(
			error,
			error_size,
			"could not start the log writer: %s",
			strerror(result)
		);
		goto destroy_wake;
	}
	writer = state;
	return 0;

destroy_wake:
	(void)pthread_cond_destroy(&state->wake);
destroy_lock:
	(void)pthread_mutex_destroy(&state->lock);
free_state:
	free(state);
	return -1;
}

bool writer_stop(void)
{
	struct log_writer *state = writer;

	if (state == NULL)
		return true;
	writer = NULL;
	(void)pthread_mutex_lock(&state->lock);
	state->stop = true;
	(void)pthread_cond_signal(&state->wake);
	(void)pthread_mutex_unlock(&state->lock);
	if (!thread_join_within(state->thread, LOG_WRITER_STOP_US)) {
		(void)pthread_detach(state->thread);
		return false;
	}
	(void)pthread_cond_destroy(&state->wake);
	(void)pthread_mutex_destroy(&state->lock);
	free(state);
	return true;
}
