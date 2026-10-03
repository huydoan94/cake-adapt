#include "latency/parser.h"
#include "common/constants.h"
#include "common/helpers.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Advances *cursor past prefix when the text there starts with it. */
static bool skip_prefix(const char **cursor, const char *prefix)
{
	size_t length = strlen(prefix);

	if (strncmp(*cursor, prefix, length) != 0)
		return false;
	*cursor += length;
	return true;
}

/* Copies [start, end) as a string; false when empty or too long for size. */
static bool copy_token(char *destination, size_t size, const char *start, const char *end)
{
	size_t length = (size_t)(end - start);

	if (length == 0U || length >= size)
		return false;
	memcpy(destination, start, length);
	destination[length] = '\0';
	return true;
}

/* fping's RTT halved into each one-way delay, as cake-autorate records it. */
static int64_t half_round_trip(double round_trip_milliseconds)
{
	double microseconds = round_trip_milliseconds * (double)MICROSECONDS_PER_MILLISECOND;

	if (microseconds > (double)UINT32_MAX)
		return (int64_t)(UINT32_MAX / 2U);
	return (int64_t)((uint32_t)(microseconds + 0.5) / 2U);
}

static bool parse_timestamp(const char *line, const char **remainder,
			    uint64_t *timestamp_microseconds)
{
	const char *closing_bracket;
	const char *decimal_point;
	char fraction_digits[7] = FRACTION_ZEROES;
	uint64_t fraction;
	uint64_t seconds;
	size_t digit_count;

	if (line[0] != '[')
		return false;
	closing_bracket = strchr(line + 1, ']');
	if (closing_bracket == NULL || closing_bracket[1] != ' ')
		return false;
	decimal_point = memchr(line + 1, '.', (size_t)(closing_bracket - (line + 1)));
	if (decimal_point == NULL || !parse_unsigned(line + 1, decimal_point, &seconds))
		return false;

	digit_count = strspn(decimal_point + 1, DECIMAL_DIGITS);
	if (digit_count == 0U || decimal_point + 1 + digit_count != closing_bracket)
		return false;
	/* Pad or truncate to six digits without rounding epoch time through float. */
	memcpy(fraction_digits, decimal_point + 1, digit_count < 6U ? digit_count : 6U);
	fraction = strtoul(fraction_digits, NULL, 10);

	if (seconds > (UINT64_MAX - fraction) / MICROSECONDS_PER_SECOND)
		return false;
	*timestamp_microseconds = seconds * MICROSECONDS_PER_SECOND + fraction;
	*remainder = closing_bracket + 2;
	return true;
}

/* Reads "<name><milliseconds>" and the separating space, if any. */
static bool parse_icmp_timestamp(const char **cursor, const char *name, uint64_t *milliseconds)
{
	const char *digits = *cursor;
	const char *end;

	if (!skip_prefix(&digits, name))
		return false;
	end = digits + strspn(digits, DECIMAL_DIGITS);
	if (!parse_unsigned(digits, end, milliseconds) || *milliseconds > UINT32_MAX ||
	    (*end != '\0' && *end != ' ')) {
		return false;
	}
	*cursor = *end == ' ' ? end + 1 : end;
	return true;
}

/*
 * ICMP timestamps are milliseconds past midnight UTC on each clock, so like
 * cake-autorate: download = Localreceive - Transmit and upload = Receive -
 * Originate, scaled to microseconds. Unsynchronized midnight rollovers produce
 * huge deltas, which the tracker resets on because the sample is marked.
 */
static bool parse_icmp_timestamps(const char *cursor, struct latency_sample *sample)
{
	uint64_t originate;
	uint64_t receive;
	uint64_t transmit;
	uint64_t local_receive;

	cursor = strstr(cursor, FPING_TIMESTAMPS_PREFIX);
	if (cursor == NULL || !skip_prefix(&cursor, FPING_TIMESTAMPS_PREFIX) ||
	    !parse_icmp_timestamp(&cursor, "Originate=", &originate) ||
	    !parse_icmp_timestamp(&cursor, "Receive=", &receive) ||
	    !parse_icmp_timestamp(&cursor, "Transmit=", &transmit) ||
	    !parse_icmp_timestamp(&cursor, "Localreceive=", &local_receive)) {
		return false;
	}
	sample->download_owd_microseconds = ((int64_t)local_receive - (int64_t)transmit) *
					    (int64_t)MICROSECONDS_PER_MILLISECOND;
	sample->upload_owd_microseconds =
		((int64_t)receive - (int64_t)originate) * (int64_t)MICROSECONDS_PER_MILLISECOND;
	sample->timestamp_rollover_sensitive = true;
	return true;
}

static enum latency_fping_line_result parse_fping_reply(const char *line, bool icmp_timestamps,
							struct latency_sample *sample)
{
	const char *cursor;
	const char *separator;
	const char *end;
	char *rtt_end;
	uint64_t sequence;
	uint64_t timestamp_microseconds;
	double round_trip_milliseconds;

	if (line == NULL || sample == NULL ||
	    !parse_timestamp(line, &cursor, &timestamp_microseconds))
		return LATENCY_FPING_LINE_INVALID;
	/* The bracketed token ends two bytes before cursor, at "] ". */
	if (!copy_token(sample->timestamp_text, sizeof(sample->timestamp_text), line, cursor - 1))
		return LATENCY_FPING_LINE_INVALID;

	separator = strstr(cursor, FPING_SEQUENCE_SEPARATOR);
	if (separator == NULL)
		return LATENCY_FPING_LINE_INVALID;
	end = separator;
	while (end > cursor && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	if (!copy_token(sample->target, sizeof(sample->target), cursor, end))
		return LATENCY_FPING_LINE_INVALID;

	cursor = separator + strlen(FPING_SEQUENCE_SEPARATOR);
	end = strchr(cursor, ']');
	if (end == NULL || !parse_unsigned(cursor, end, &sequence))
		return LATENCY_FPING_LINE_INVALID;
	sample->download_owd_microseconds = 0U;
	sample->upload_owd_microseconds = 0U;
	sample->timestamp_microseconds = timestamp_microseconds;
	sample->timestamp_rollover_sensitive = false;
	sample->sequence = sequence;
	cursor = end + 1;
	if (skip_prefix(&cursor, FPING_TIMEOUT_SUFFIX))
		return LATENCY_FPING_LINE_TIMEOUT;
	if (!skip_prefix(&cursor, FPING_FIELD_SEPARATOR))
		return LATENCY_FPING_LINE_INVALID;

	end = cursor + strspn(cursor, DECIMAL_DIGITS);
	if (end == cursor)
		return LATENCY_FPING_LINE_INVALID;
	cursor = end;
	if (!skip_prefix(&cursor, FPING_BYTES_SEPARATOR))
		return LATENCY_FPING_LINE_INVALID;
	errno = 0;
	round_trip_milliseconds = strtod(cursor, &rtt_end);
	if (errno == ERANGE || rtt_end == cursor || !isfinite(round_trip_milliseconds) ||
	    round_trip_milliseconds < 0.0 ||
	    strncmp(rtt_end, FPING_MILLISECONDS_SUFFIX, strlen(FPING_MILLISECONDS_SUFFIX)) != 0)
		return LATENCY_FPING_LINE_INVALID;
	if (icmp_timestamps)
		return parse_icmp_timestamps(rtt_end, sample) ? LATENCY_FPING_LINE_SAMPLE
							      : LATENCY_FPING_LINE_INVALID;

	sample->download_owd_microseconds = half_round_trip(round_trip_milliseconds);
	sample->upload_owd_microseconds = sample->download_owd_microseconds;
	return LATENCY_FPING_LINE_SAMPLE;
}

enum latency_fping_line_result parse_fping_line(const char *line, struct latency_sample *sample)
{
	return parse_fping_reply(line, false, sample);
}

enum latency_fping_line_result parse_fping_timestamp_line(const char *line,
							  struct latency_sample *sample)
{
	return parse_fping_reply(line, true, sample);
}

static bool token_has_unit(const char *token, const char *unit)
{
	size_t length = strlen(unit);

	return strncmp(token, unit, length) == 0 &&
	       (token[length] == '\0' || token[length] == ' ' || token[length] == '\t');
}

static bool parse_irtt_duration(const char *value, int64_t *microseconds)
{
	char *unit;
	double parsed;
	double scale;
	double converted;

	errno = 0;
	parsed = strtod(value, &unit);
	if (errno == ERANGE || unit == value || !isfinite(parsed) || parsed < 0.0)
		return false;
	if (token_has_unit(unit, "ns"))
		scale = 1.0 / (double)THOUSAND;
	else if (token_has_unit(unit, "us") || token_has_unit(unit, "µs"))
		scale = (double)MICROSECOND;
	else if (token_has_unit(unit, "ms"))
		scale = (double)MILLISECOND;
	else if (token_has_unit(unit, "s"))
		scale = (double)SECOND;
	else
		return false;
	converted = parsed * scale;
	if (!isfinite(converted) || converted >= (double)INT64_MAX)
		return false;
	*microseconds = (int64_t)(converted + 0.5);
	return true;
}

static bool irtt_value(const char *line, const char *name, const char **value)
{
	size_t name_length = strlen(name);
	const char *cursor = line;

	while (*cursor != '\0') {
		const char *end = strpbrk(cursor, " \t");
		size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);

		if (length > name_length && strncmp(cursor, name, name_length) == 0 &&
		    cursor[name_length] == '=') {
			*value = cursor + name_length + 1U;
			return true;
		}
		if (end == NULL)
			break;
		cursor = end + strspn(end, " \t");
	}
	return false;
}

bool parse_irtt_line(const char *line, const char *target, uint64_t timestamp_microseconds,
		     struct latency_sample *sample)
{
	const char *sequence_text;
	const char *download_text;
	const char *upload_text;
	char *sequence_end;
	uintmax_t sequence;
	int64_t download;
	int64_t upload;

	if (line == NULL || target == NULL || sample == NULL ||
	    !irtt_value(line, "seq", &sequence_text) || !irtt_value(line, "rd", &download_text) ||
	    !irtt_value(line, "sd", &upload_text)) {
		return false;
	}
	errno = 0;
	sequence = strtoumax(sequence_text, &sequence_end, 10);
	if (errno == ERANGE || sequence_end == sequence_text ||
	    (*sequence_end != '\0' && *sequence_end != ' ' && *sequence_end != '\t') ||
	    !parse_irtt_duration(download_text, &download) ||
	    !parse_irtt_duration(upload_text, &upload)) {
		return false;
	}
	if (strlen(target) >= sizeof(sample->target))
		return false;
	*sample = (struct latency_sample){ .download_owd_microseconds = download,
					   .upload_owd_microseconds = upload,
					   .timestamp_microseconds = timestamp_microseconds,
					   .timestamp_rollover_sensitive = false,
					   .sequence = (uint64_t)sequence };
	(void)strcpy(sample->target, target);
	return true;
}
