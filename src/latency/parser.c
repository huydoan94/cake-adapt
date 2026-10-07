#include "latency/parser.h"
#include "common/constants.h"
#include "common/helpers.h"
#include "common/utils.h"

#include <ctype.h>
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

/* fping's RTT halved into each one-way delay, rounded to the microsecond. */
static int64_t half_round_trip(uint64_t round_trip_ns)
{
	uint64_t half = rounded_divide(round_trip_ns, 2U * NANOSECONDS_PER_US);

	return half > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)half;
}

/* fping's RTT digits: whole milliseconds, then up to six decimals kept, the next rounding. */
#define ROUND_TRIP_FRACTION_DIGITS 6U

/* Reads fping's decimal RTT in milliseconds exactly, as nanoseconds. */
static bool parse_round_trip(const char *text, const char **end, uint64_t *round_trip_ns)
{
	const char *cursor = text + strspn(text, DECIMAL_DIGITS);
	uint64_t ms;
	uint64_t fraction = 0U;

	if (!parse_unsigned(text, cursor, &ms) || ms >= UINT64_MAX / NANOSECONDS_PER_MILLISECOND)
		return false;
	if (*cursor == '.') {
		const char *digits = cursor + 1;
		size_t count = strspn(digits, DECIMAL_DIGITS);

		if (count == 0U)
			return false;
		for (size_t index = 0U; index < ROUND_TRIP_FRACTION_DIGITS; index++)
			fraction = fraction * 10U +
				   (index < count ? (uint64_t)(digits[index] - '0') : 0U);
		if (count > ROUND_TRIP_FRACTION_DIGITS && digits[ROUND_TRIP_FRACTION_DIGITS] >= '5')
			fraction++;
		cursor = digits + count;
	}
	*round_trip_ns = ms * NANOSECONDS_PER_MILLISECOND + fraction;
	*end = cursor;
	return true;
}

/* Microseconds have six fraction digits. */
#define FRACTION_DIGITS (sizeof(FRACTION_ZEROES) - 1U)

static bool parse_timestamp(const char *line, const char **remainder, uint64_t *timestamp_us)
{
	const char *closing_bracket;
	const char *decimal_point;
	char fraction_digits[sizeof(FRACTION_ZEROES)] = FRACTION_ZEROES;
	uint64_t fraction;
	uint64_t sec;
	size_t digit_count;

	if (line[0] != '[')
		return false;
	closing_bracket = strchr(line + 1, ']');
	if (closing_bracket == NULL || closing_bracket[1] != ' ')
		return false;
	decimal_point = memchr(line + 1, '.', (size_t)(closing_bracket - (line + 1)));
	if (decimal_point == NULL || !parse_unsigned(line + 1, decimal_point, &sec))
		return false;

	digit_count = strspn(decimal_point + 1, DECIMAL_DIGITS);
	if (digit_count == 0U || decimal_point + 1 + digit_count != closing_bracket)
		return false;
	/* Pad or round to six digits without passing epoch time through float. */
	memcpy(fraction_digits,
	       decimal_point + 1,
	       digit_count < FRACTION_DIGITS ? digit_count : FRACTION_DIGITS);
	fraction = strtoul(fraction_digits, NULL, 10);
	if (digit_count > FRACTION_DIGITS && decimal_point[1 + FRACTION_DIGITS] >= '5')
		fraction++;

	if (sec > (UINT64_MAX - fraction) / US_PER_SECOND)
		return false;
	*timestamp_us = sec * US_PER_SECOND + fraction;
	*remainder = closing_bracket + 2;
	return true;
}

/* Reads "<name><milliseconds>" and the separating space, if any. */
static bool parse_icmp_timestamp(const char **cursor, const char *name, uint64_t *ms)
{
	const char *digits = *cursor;
	const char *end;

	if (!skip_prefix(&digits, name))
		return false;
	end = digits + strspn(digits, DECIMAL_DIGITS);
	if (!parse_unsigned(digits, end, ms) || *ms > UINT32_MAX || (*end != '\0' && *end != ' '))
		return false;
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
	    !parse_icmp_timestamp(&cursor, FPING_ORIGINATE, &originate) ||
	    !parse_icmp_timestamp(&cursor, FPING_RECEIVE, &receive) ||
	    !parse_icmp_timestamp(&cursor, FPING_TRANSMIT, &transmit) ||
	    !parse_icmp_timestamp(&cursor, FPING_LOCAL_RECEIVE, &local_receive)) {
		return false;
	}
	sample->download_owd_us =
		((int64_t)local_receive - (int64_t)transmit) * (int64_t)US_PER_MILLISECOND;
	sample->upload_owd_us =
		((int64_t)receive - (int64_t)originate) * (int64_t)US_PER_MILLISECOND;
	sample->timestamp_rollover_sensitive = true;
	return true;
}

/*
 * The part every fping reply shares: timestamp, target, sequence and RTT.
 * On a sample, *tail points past the RTT's "ms" for the caller to finish.
 */
static enum latency_fping_line_result parse_fping_reply(
	const char *line,
	struct latency_sample *sample,
	uint64_t *round_trip_ns,
	const char **tail
)
{
	const char *cursor;
	const char *separator;
	const char *end;
	uint64_t sequence;
	uint64_t timestamp_us;

	if (!parse_timestamp(line, &cursor, &timestamp_us))
		return LATENCY_FPING_LINE_INVALID;
	/* The bracketed token ends two bytes before cursor, at "] ". */
	if (!copy_token(sample->timestamp_text, sizeof(sample->timestamp_text), line, cursor - 1))
		return LATENCY_FPING_LINE_INVALID;

	separator = strstr(cursor, FPING_SEQUENCE_SEPARATOR);
	if (separator == NULL)
		return LATENCY_FPING_LINE_INVALID;
	end = separator;
	while (end > cursor && isblank((unsigned char)end[-1]))
		end--;
	if (!copy_token(sample->target, sizeof(sample->target), cursor, end))
		return LATENCY_FPING_LINE_INVALID;

	cursor = separator + strlen(FPING_SEQUENCE_SEPARATOR);
	end = strchr(cursor, ']');
	if (end == NULL || !parse_unsigned(cursor, end, &sequence))
		return LATENCY_FPING_LINE_INVALID;
	sample->download_owd_us = 0U;
	sample->upload_owd_us = 0U;
	sample->timestamp_us = timestamp_us;
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
	if (!parse_round_trip(cursor, tail, round_trip_ns) || !skip_prefix(tail, FPING_MS_SUFFIX))
		return LATENCY_FPING_LINE_INVALID;
	return LATENCY_FPING_LINE_SAMPLE;
}

enum latency_fping_line_result parse_fping_line(const char *line, struct latency_sample *sample)
{
	uint64_t round_trip_ns;
	const char *tail;
	enum latency_fping_line_result result =
		parse_fping_reply(line, sample, &round_trip_ns, &tail);

	if (result != LATENCY_FPING_LINE_SAMPLE)
		return result;
	sample->download_owd_us = half_round_trip(round_trip_ns);
	sample->upload_owd_us = sample->download_owd_us;
	return LATENCY_FPING_LINE_SAMPLE;
}

enum latency_fping_line_result
parse_fping_timestamp_line(const char *line, struct latency_sample *sample)
{
	uint64_t round_trip_ns;
	const char *tail;
	enum latency_fping_line_result result =
		parse_fping_reply(line, sample, &round_trip_ns, &tail);

	if (result != LATENCY_FPING_LINE_SAMPLE)
		return result;
	return parse_icmp_timestamps(tail, sample) ? LATENCY_FPING_LINE_SAMPLE :
						     LATENCY_FPING_LINE_INVALID;
}

/* token starts with unit, followed by the end of the value. */
static bool token_has_unit(const char *token, const char *unit)
{
	return skip_prefix(&token, unit) && (*token == '\0' || isblank((unsigned char)*token));
}

static bool parse_irtt_duration(const char *value, int64_t *us)
{
	char *unit;
	double parsed;
	double scale;
	double converted;

	errno = 0;
	parsed = strtod(value, &unit);
	if (errno == ERANGE || unit == value || !isfinite(parsed) || parsed < 0.0)
		return false;
	if (token_has_unit(unit, IRTT_UNIT_NS))
		scale = 1.0 / (double)THOUSAND;
	else if (token_has_unit(unit, IRTT_UNIT_US) || token_has_unit(unit, IRTT_UNIT_US_SIGN))
		scale = (double)US;
	else if (token_has_unit(unit, IRTT_UNIT_MS))
		scale = (double)MILLISECOND;
	else if (token_has_unit(unit, IRTT_UNIT_SEC))
		scale = (double)SECOND;
	else
		return false;
	converted = parsed * scale;
	if (!isfinite(converted) || converted >= (double)INT64_MAX)
		return false;
	*us = (int64_t)(converted + 0.5);
	return true;
}

static bool irtt_value(const char *line, const char *name, const char **value)
{
	size_t name_length = strlen(name);
	const char *cursor = line;

	while (*cursor != '\0') {
		const char *end = strpbrk(cursor, BLANK_CHARACTERS);
		size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);

		if (length > name_length && strncmp(cursor, name, name_length) == 0 &&
		    cursor[name_length] == '=') {
			*value = cursor + name_length + 1U;
			return true;
		}
		if (end == NULL)
			break;
		cursor = end + strspn(end, BLANK_CHARACTERS);
	}
	return false;
}

bool parse_irtt_line(const char *line, const char *target, struct latency_sample *sample)
{
	const char *sequence_text;
	const char *download_text;
	const char *upload_text;
	char *sequence_end;
	uintmax_t sequence;
	int64_t download;
	int64_t upload;

	if (!irtt_value(line, IRTT_SEQUENCE, &sequence_text) ||
	    !irtt_value(line, IRTT_RECEIVE_DELAY, &download_text) ||
	    !irtt_value(line, IRTT_SEND_DELAY, &upload_text)) {
		return false;
	}
	errno = 0;
	sequence = strtoumax(sequence_text, &sequence_end, 10);
	if (errno == ERANGE || sequence_end == sequence_text ||
	    (*sequence_end != '\0' && !isblank((unsigned char)*sequence_end)) ||
	    !parse_irtt_duration(download_text, &download) ||
	    !parse_irtt_duration(upload_text, &upload)) {
		return false;
	}
	*sample = (struct latency_sample){ .download_owd_us = download,
					   .upload_owd_us = upload,
					   .timestamp_rollover_sensitive = false,
					   .sequence = (uint64_t)sequence };
	return copy_token(sample->target, sizeof(sample->target), target, target + strlen(target));
}
