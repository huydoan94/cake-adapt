#!/bin/sh
# host-test.sh: runs the host test suites (tests/Makefile check, check-netlink,
# check-config and check-tcpdelay), each to the end, then says which failed
# and why, or why they could not run.
#
# Usage: sh tools/host-test.sh [NAME|DIRECTORY]
#
# The uci.h, libubox and libbpf headers come from an OpenWrt SDK's staged
# includes: NAME or DIRECTORY as sdk-resolve.sh finds it, x86 by default.
# libnl-3 and zlib come from the host. The full output is shown and also kept
# in build/host-test.log.
#
# Exit status: 0 all passed, 2 SDK or host headers missing, 4 compile error,
# 5 a test failed, 1 any other failure.
set -u

repo=$(cd "$(dirname "$0")/.." && pwd)
. "$repo/tools/sdk-resolve.sh"
sdk_resolve "${1:-x86}"

fail() { # STATUS MESSAGE...
	status=$1
	shift
	if [ "$status" -eq 5 ]; then
		printf '\nTESTS FAILED: ' >&2
	else
		printf '\nCANNOT TEST: ' >&2
	fi
	printf '%s\n' "$@" >&2
	exit "$status"
}

command -v "${CC:-cc}" >/dev/null || fail 2 "no C compiler (${CC:-cc}); install gcc"
command -v pkg-config >/dev/null || fail 2 "no pkg-config; install pkg-config"
pkg-config --exists libnl-3.0 ||
	fail 2 "the host has no libnl-3 development files; install libnl-3-dev and libnl-route-3-dev"
sdk_check
headers=$(ls -d "$sdk"/staging_dir/target-*/usr/include 2>/dev/null | head -n 1)
[ -n "$headers" ] && [ -f "$headers/uci.h" ] && [ -d "$headers/libubox" ] ||
	fail 2 "the SDK at $sdk has no staged uci.h and libubox headers" \
		"Build them once with: make -C $sdk package/cake-adapt/compile"

mkdir -p "$repo/build"
log=$repo/build/host-test.log
: >"$log"
failed=
for suite in check check-netlink check-config check-tcpdelay; do
	echo "== $suite" | tee -a "$log"
	if make -C "$repo/tests" "$suite" UCI_CFLAGS="-isystem $headers" >>"$log.part" 2>&1; then
		result=passed
	else
		result=FAILED
		failed="$failed $suite"
	fi
	cat "$log.part"
	cat "$log.part" >>"$log"
	rm -f "$log.part"
	echo "== $suite $result" | tee -a "$log"
done

[ -n "$failed" ] || {
	printf '\nALL PASSED (headers from %s)\n' "$sdk"
	exit 0
}
missing=$(sed -n 's/.*fatal error: \([^:]*\): No such file or directory.*/\1/p' "$log" | sort -u | tr '\n' ' ')
[ -z "$missing" ] ||
	fail 2 "failed:$failed" "headers not found: $missing" \
		"netlink/* is libnl-3-dev or libnl-route-3-dev, zlib.h is zlib1g-dev, uci.h, libubox/* and bpf/* come from the SDK's staged includes."
# Suites share sources, so one error repeats once per suite.
errors=$(grep -E '^[^ ]+:[0-9]+:[0-9]+: (fatal )?error:|undefined reference' "$log" | awk '!seen[$0]++' | head -n 20)
[ -z "$errors" ] ||
	fail 4 "failed:$failed" "compile errors:" "$errors"
assertions=$(grep -E 'Assertion .* failed' "$log" | awk '!seen[$0]++' | head -n 20)
[ -z "$assertions" ] ||
	fail 5 "failed:$failed" "test failures:" "$assertions"
fail 1 "failed:$failed; see build/host-test.log"
