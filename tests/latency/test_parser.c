#define _POSIX_C_SOURCE 200809L

#include "latency/parser.h"
#include "common/constants.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_fping_reply_is_parsed(void)
{
	struct latency_sample sample;

	assert(parse_fping_line("[1789284242.09616] 1.1.1.1 : [65536], 64 bytes,"
				" 31.9 ms (31.9 avg, 0% loss)",
				&sample) == LATENCY_FPING_LINE_SAMPLE);
	assert(sample.timestamp_microseconds == UINT64_C(1789284242096160));
	/* cake-autorate logs fping's token verbatim, including its brackets. */
	assert(strcmp(sample.timestamp_text, "[1789284242.09616]") == 0);
	assert(strcmp(sample.target, "1.1.1.1") == 0);
	assert(sample.sequence == UINT64_C(65536));
	assert(sample.download_owd_microseconds == 15950U);
	assert(sample.upload_owd_microseconds == 15950U);
	assert(!sample.timestamp_rollover_sensitive);
}

/* fping 5.3's reply format with --timestamp --loop --icmp-timestamp. */
static void test_fping_icmp_timestamp_reply_is_parsed_directionally(void)
{
	struct latency_sample sample;

	assert(parse_fping_timestamp_line(
		       "[1789284242.09616] 1.1.1.1 : [7], 20 bytes, 54.1 ms (54.1 avg, 0% loss),"
		       " timestamps: Originate=60120684 Receive=60120713 Transmit=60120713"
		       " Localreceive=60120738",
		       &sample) == LATENCY_FPING_LINE_SAMPLE);
	assert(sample.timestamp_microseconds == UINT64_C(1789284242096160));
	assert(strcmp(sample.timestamp_text, "[1789284242.09616]") == 0);
	assert(strcmp(sample.target, "1.1.1.1") == 0);
	assert(sample.sequence == 7U);
	/* download = Localreceive - Transmit, upload = Receive - Originate. */
	assert(sample.download_owd_microseconds == 25000);
	assert(sample.upload_owd_microseconds == 29000);
	assert(sample.timestamp_rollover_sensitive);

	/* An unsynchronized remote clock can make a direction negative. */
	assert(parse_fping_timestamp_line(
		       "[100.5] 9.9.9.9 : [0], 20 bytes, 3.0 ms (3.0 avg, 0% loss),"
		       " timestamps: Originate=5000 Receive=4990 Transmit=4990 Localreceive=5003",
		       &sample) == LATENCY_FPING_LINE_SAMPLE);
	assert(sample.download_owd_microseconds == 13000);
	assert(sample.upload_owd_microseconds == -10000);

	/* A remote midnight rollover yields a huge delay for the tracker to reset on. */
	assert(parse_fping_timestamp_line(
		       "[100.5] 9.9.9.9 : [1], 20 bytes, 3.0 ms (3.0 avg, 0% loss),"
		       " timestamps: Originate=86399998 Receive=1 Transmit=1 Localreceive=86400000",
		       &sample) == LATENCY_FPING_LINE_SAMPLE);
	assert(sample.upload_owd_microseconds == INT64_C(-86399997000));
	assert(sample.download_owd_microseconds == INT64_C(86399999000));
	assert(parse_fping_timestamp_line(
		       "[100.5] 9.9.9.9 : [2], 20 bytes, 3.0 ms (3.0 avg, 0% loss),"
		       " timestamps: Originate=0 Receive=4294967295 Transmit=4294967295 Localreceive=0",
		       &sample) == LATENCY_FPING_LINE_SAMPLE);
	assert(sample.upload_owd_microseconds == INT64_C(4294967295000));
}

static void test_fping_icmp_timestamp_reply_rejects_malformed_fields(void)
{
	static const char prefix[] = "[100.5] 1.1.1.1 : [0], 20 bytes, 3.0 ms (3.0 avg, 0% loss),";
	static const char *const invalid[] = {
		"",
		" timestamps:",
		" timestamps: Originate=1 Receive=2 Transmit=3",
		" timestamps: Receive=2 Originate=1 Transmit=3 Localreceive=4",
		" timestamps: Originate=4294967296 Receive=2 Transmit=3 Localreceive=4",
		" timestamps: Originate=1 Receive=2 Transmit=3 Localreceive=4x",
		" timestamps: Originate=-1 Receive=2 Transmit=3 Localreceive=4",
		" timestamps: Originate= Receive=2 Transmit=3 Localreceive=4"
	};
	struct latency_sample sample;
	char line[256];
	size_t index;

	for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
		(void)snprintf(line, sizeof(line), "%s%s", prefix, invalid[index]);
		assert(parse_fping_timestamp_line(line, &sample) == LATENCY_FPING_LINE_INVALID);
	}
	assert(parse_fping_timestamp_line("[100.8] 1.1.1.1 : [1], timed out (NaN avg, 50% loss)",
					  &sample) == LATENCY_FPING_LINE_TIMEOUT);
	assert(sample.sequence == 1U);
}

static void test_irtt_reply_is_parsed_directionally(void)
{
	struct latency_sample sample;

	assert(parse_irtt_line("seq=42 rtt=3ms rd=1.234ms sd=567µs ipdv=0s", "2001:db8::1",
			       UINT64_C(123456789), &sample));
	assert(strcmp(sample.target, "2001:db8::1") == 0);
	assert(sample.sequence == 42U);
	assert(sample.download_owd_microseconds == 1234);
	assert(sample.upload_owd_microseconds == 567);
	assert(sample.timestamp_microseconds == UINT64_C(123456789));
	assert(!sample.timestamp_rollover_sensitive);

	assert(parse_irtt_line("seq=9 rd=1500ns sd=2s", "1.1.1.1", 1U, &sample));
	assert(sample.download_owd_microseconds == 2);
	assert(sample.upload_owd_microseconds == 2 * (int64_t)SECOND);
	assert(!parse_irtt_line("seq=9 rd=-1ms sd=2ms", "1.1.1.1", 1U, &sample));
	assert(!parse_irtt_line("seq=9 rd=1ms", "1.1.1.1", 1U, &sample));
}

static void test_fping_six_digit_timestamp_is_preserved(void)
{
	struct latency_sample sample;

	assert(parse_fping_line("[1789284242.000123] 9.9.9.9 : [7], 64 bytes,"
				" 0.125 ms (0.125 avg, 0% loss)",
				&sample) == LATENCY_FPING_LINE_SAMPLE);
	assert(sample.timestamp_microseconds == UINT64_C(1789284242000123));
	assert(strcmp(sample.target, "9.9.9.9") == 0);
	assert(sample.sequence == 7U);
	assert(sample.download_owd_microseconds == 62U);
	assert(sample.upload_owd_microseconds == 62U);
}

static void test_fping_odd_and_extreme_rtt_use_equal_owd_halves(void)
{
	struct latency_sample sample;

	assert(parse_fping_line("[1.000001] 1.1.1.1 : [1], 64 bytes, 1.001 ms", &sample) ==
	       LATENCY_FPING_LINE_SAMPLE);
	assert(sample.download_owd_microseconds == 500U);
	assert(sample.upload_owd_microseconds == 500U);

	assert(parse_fping_line("[1.000001] 1.1.1.1 : [2], 64 bytes, 4294967.296 ms", &sample) ==
	       LATENCY_FPING_LINE_SAMPLE);
	assert(sample.download_owd_microseconds == UINT32_MAX / 2U);
	assert(sample.upload_owd_microseconds == UINT32_MAX / 2U);
}

static void test_fping_byte_count_syntax(void)
{
	const char *const invalid[] = { "[123.000001] 1.1.1.1 : [1],  bytes, 1.0 ms",
					"[123.000001] 1.1.1.1 : [1], -64 bytes, 1.0 ms",
					"[123.000001] 1.1.1.1 : [1], 64x bytes, 1.0 ms",
					"[123.000001] 1.1.1.1 : [1], 64 byte, 1.0 ms" };
	struct latency_sample sample;

	for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); index++)
		assert(parse_fping_line(invalid[index], &sample) == LATENCY_FPING_LINE_INVALID);
	assert(parse_fping_line("[123.000001] 1.1.1.1 : [1], 0 bytes, 1.0 ms", &sample) ==
	       LATENCY_FPING_LINE_SAMPLE);
}

static void test_fping_timestamp_boundaries(void)
{
	const struct {
		const char *text;
		uint64_t microseconds;
	} valid[] = { { "0.1", 100000U },
		      { "12.00000123", 12000001U },
		      { "12.12345678901234567890", 12123456U },
		      { "18446744073709.551615", UINT64_MAX } };
	const char *const invalid[] = { "12.",
					"12.1x",
					"12.-1",
					".1",
					"12. 1",
					"18446744073709.551616",
					"18446744073709551616.0" };
	struct latency_sample sample;
	char line[128];

	for (size_t index = 0U; index < sizeof(valid) / sizeof(valid[0]); index++) {
		(void)snprintf(line, sizeof(line), "[%s] 1.1.1.1 : [1], 64 bytes, 1.0 ms",
			       valid[index].text);
		assert(parse_fping_line(line, &sample) == LATENCY_FPING_LINE_SAMPLE);
		assert(sample.timestamp_microseconds == valid[index].microseconds);
		assert(strncmp(sample.timestamp_text + 1, valid[index].text,
			       strlen(valid[index].text)) == 0);
	}
	/* A token that cannot be logged verbatim is not fping output. */
	(void)snprintf(line, sizeof(line), "[1.%045d] 1.1.1.1 : [1], 64 bytes, 1.0 ms", 0);
	assert(parse_fping_line(line, &sample) == LATENCY_FPING_LINE_INVALID);
	for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
		(void)snprintf(line, sizeof(line), "[%s] 1.1.1.1 : [1], 64 bytes, 1.0 ms",
			       invalid[index]);
		assert(parse_fping_line(line, &sample) == LATENCY_FPING_LINE_INVALID);
	}
}

static void test_fping_timeout_is_recognized(void)
{
	struct latency_sample sample;

	assert(parse_fping_line("[1789284242.09616] 1.1.1.1 : [8], timed out"
				" (NaN avg, 100% loss)",
				&sample) == LATENCY_FPING_LINE_TIMEOUT);
	assert(strcmp(sample.target, "1.1.1.1") == 0);
	assert(sample.sequence == 8U);
}

static void test_fping_reply_identifies_each_target(void)
{
	struct latency_sample sample;

	assert(parse_fping_line("[1789284242.09616] 9.9.9.9  : [8], 64 bytes,"
				" 31.9 ms (31.9 avg, 0% loss)",
				&sample) == LATENCY_FPING_LINE_SAMPLE);
	assert(strcmp(sample.target, "9.9.9.9") == 0);
}

int main(void)
{
	test_fping_icmp_timestamp_reply_is_parsed_directionally();
	test_fping_icmp_timestamp_reply_rejects_malformed_fields();
	test_fping_reply_is_parsed();
	test_irtt_reply_is_parsed_directionally();
	test_fping_six_digit_timestamp_is_preserved();
	test_fping_byte_count_syntax();
	test_fping_odd_and_extreme_rtt_use_equal_owd_halves();
	test_fping_timestamp_boundaries();
	test_fping_timeout_is_recognized();
	test_fping_reply_identifies_each_target();

	(void)puts("latency parser tests passed");
	return 0;
}
