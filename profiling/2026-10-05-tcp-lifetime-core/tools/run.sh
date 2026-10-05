#!/bin/sh
set -eu
test_dir=$1
result_dir=$(mktemp -d /root/cake-adapt-lifetime-dry.XXXXXX)
namespace=cake-lifetime-dry-$$
namespace_owned=0
log_link_owned=0
log_dir=
test_log=/tmp/sqm-mon-test.log
cleanup()
{
	if [ "$namespace_owned" -eq 1 ]; then ip netns del "$namespace"; fi
	if [ "$log_link_owned" -eq 1 ]; then rm -f "$log_dir/cake-adapt.log"; fi
	if [ -n "$log_dir" ]; then rmdir "$log_dir"; fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
snapshot()
{
	phase=$1
	{ uname -a; cat /etc/openwrt_release; apk info -a cake-adapt; } > "$result_dir/$phase.package"
	{ pidof cake-adapt || true; /etc/init.d/cake-adapt status || true; } > "$result_dir/$phase.process"
	{ ip -o link show; ip rule show; ip -4 route show table all; ip -6 route show table all; tc -d qdisc show; ip netns list; } > "$result_dir/$phase.network"
	ip -d link show > "$result_dir/$phase.link-details"
	tc -s -d qdisc show > "$result_dir/$phase.qdisc-counters"
	for file in /etc/config/cake-adapt /etc/init.d/cake-adapt /usr/sbin/cake-adapt /lib/bpf/cake-adapt-tcpdelay.o; do
		if [ -f "$file" ]; then sha256sum "$file"; fi
	done > "$result_dir/$phase.files"
}
snapshot before
# This dry run requires an absent log or a log without open descriptors.
# Refuse even read-only holders rather than guessing access flags without stat.
for descriptor in /proc/[0-9]*/fd/*; do
	if [ "$(readlink "$descriptor" 2>/dev/null || true)" = "$test_log" ]; then
		printf 'Existing test-log holder: %s\n' "$descriptor" >&2
		exit 1
	fi
done
if [ ! -e "$test_log" ]; then touch "$test_log"; fi
if [ -L "$test_log" ] || [ ! -f "$test_log" ]; then exit 1; fi
set -- $(ls -di "$test_log")
inode=$1
printf '%s\n' "$inode" > "$result_dir/log-inode.before"
: > "$test_log"
log_dir=$(mktemp -d /tmp/cake-adapt-lifetime-log.XXXXXX)
ln "$test_log" "$log_dir/cake-adapt.log"
log_link_owned=1
sha256sum "$test_dir/runner" "$test_dir/candidate.o" > "$result_dir/input-checksums"
ip netns add "$namespace"
namespace_owned=1
ip -n "$namespace" link add ca0 type veth peer name ca1
ip -n "$namespace" link set ca0 address 02:00:00:00:00:01
ip -n "$namespace" link set ca1 address 02:00:00:00:00:02
ip -n "$namespace" link set ca0 up
ip -n "$namespace" link set ca1 up
# The runner has its own 120-second alarm; it loads only this isolated object.
run_rc=0
ip netns exec "$namespace" "$test_dir/runner" "$test_dir/candidate.o" ca0 ca1 > "$result_dir/runner.stdout" 2> "$result_dir/runner.stderr" || run_rc=$?
printf '%s\n' "$run_rc" > "$result_dir/runner.exit-code"
ip netns del "$namespace"
namespace_owned=0
snapshot after
set -- $(ls -di "$test_log")
printf '%s\n' "$1" > "$result_dir/log-inode.after"
state_rc=0
for state in package process network files; do
	if cmp -s "$result_dir/before.$state" "$result_dir/after.$state"; then
		printf '%s unchanged\n' "$state"
	else
		printf '%s CHANGED\n' "$state"
		state_rc=1
	fi
done
cmp -s "$result_dir/log-inode.before" "$result_dir/log-inode.after" || state_rc=1
printf 'RESULT_DIR=%s RUN_RC=%s STATE_RC=%s\n' "$result_dir" "$run_rc" "$state_rc"
if [ "$run_rc" -ne 0 ]; then exit "$run_rc"; fi
exit "$state_rc"
