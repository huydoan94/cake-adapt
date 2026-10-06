#!/bin/sh
set -eu

umask 077

HARNESS_DIR=/tmp/cake-adapt-tcp-lifetime-capacity
RUNNER=$HARNESS_DIR/runner
OBJECT=${1:-$HARNESS_DIR/cake-adapt-tcpdelay.o}
WRAPPER=$HARNESS_DIR/run.sh
TEST_LOG=/tmp/sqm-mon-test.log
LOCK=/tmp/cake-adapt-tcp-lifetime-capacity-evidence.lock
LOG_LINK=
LOG_DIR=
RESULT_DIR=
LOCK_OWNED=0
LOG_LINK_OWNED=0
LOG_DIR_OWNED=0
LOG_DEVICE_INODE=
NAMESPACE_RUN_STARTED=0
RUN_RC=125
AFTER_CAPTURED=0

fail()
{
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

have()
{
	command -v "$1" >/dev/null 2>&1 || fail "required command unavailable: $1"
}

log_identity()
{
	set -- $(ls -di "$TEST_LOG")
	printf '%s\n' "$1"
}

check_log_writers()
{
	log_id=$1
	for proc_dir in /proc/[0-9]*; do
		[ -d "$proc_dir/fd" ] || continue
		proc_pid=${proc_dir#/proc/}
		for fd_path in "$proc_dir"/fd/*; do
			[ -e "$fd_path" ] || continue
			fd_target=$(readlink "$fd_path" 2>/dev/null) || continue
			[ "$fd_target" = "$TEST_LOG" ] || continue
			fd_number=${fd_path##*/}
			fd_flags=$(sed -n 's/^flags:[[:space:]]*//p' \
				"$proc_dir/fdinfo/$fd_number" 2>/dev/null | head -n 1)
			[ -n "$fd_flags" ] || fail "cannot inspect log fd flags for PID $proc_pid fd $fd_number"
			fd_access=$((0$fd_flags & 3))
			[ "$fd_access" -eq 0 ] && continue
			proc_name=$(tr '\000' ' ' < "$proc_dir/cmdline" 2>/dev/null || true)
			fail "writer already holds $TEST_LOG (pid=$proc_pid fd=$fd_number cmd=$proc_name)"
		done
	done
}

namespace_list()
{
	ip netns list | sed 's/[[:space:]].*$//'
}

capture_namespaces()
{
	namespace_list | sort
}

capture_state()
{
	state_dir=$1
	mkdir -p "$state_dir"

	{
		printf '%s\n' '=== service init status ==='
		if [ -x /etc/init.d/cake-adapt ]; then
			if /etc/init.d/cake-adapt status; then status_rc=0; else status_rc=$?; fi
			printf 'status_rc=%s\n' "$status_rc"
		else
			printf '%s\n' 'init script absent'
		fi
		printf '%s\n' '=== ubus service state ==='
		if command -v ubus >/dev/null 2>&1; then
			if ubus call service list '{"name":"cake-adapt"}'; then :; else
				printf 'ubus_rc=%s\n' "$?"
			fi
		else
			printf '%s\n' 'ubus unavailable'
		fi
	} > "$state_dir/service.txt" 2>&1

	{
		printf '%s\n' '=== cake-adapt PIDs ==='
		if pidof cake-adapt; then :; else printf '%s\n' 'none'; fi
		for proc_dir in /proc/[0-9]*; do
			[ -r "$proc_dir/comm" ] || continue
			proc_name=$(cat "$proc_dir/comm" 2>/dev/null || true)
			[ "$proc_name" = cake-adapt ] || continue
			printf '=== PID %s ===\n' "${proc_dir#/proc/}"
			printf 'exe='
			readlink "$proc_dir/exe" 2>/dev/null || true
			printf 'cmdline='
			tr '\000' ' ' < "$proc_dir/cmdline" 2>/dev/null || true
			printf '\n'
		cat "$proc_dir/status" 2>/dev/null || true
		done
	} > "$state_dir/processes.txt" 2>&1

	{
		for proc_dir in /proc/[0-9]*; do
			[ -r "$proc_dir/comm" ] || continue
			proc_name=$(cat "$proc_dir/comm" 2>/dev/null || true)
			[ "$proc_name" = cake-adapt ] || continue
			printf 'pid=%s exe=' "${proc_dir#/proc/}"
			readlink "$proc_dir/exe" 2>/dev/null || true
			printf 'cmdline='
			tr '\000' ' ' < "$proc_dir/cmdline" 2>/dev/null || true
			printf '\n'
		done
	} > "$state_dir/processes.stable.txt" 2>&1

	{
		printf '%s\n' '=== package status ==='
		if command -v apk >/dev/null 2>&1; then
			if apk info -a cake-adapt; then :; else printf 'apk_info_rc=%s\n' "$?"; fi
			if apk info -W /usr/sbin/cake-adapt; then :; else
				printf 'apk_owner_rc=%s\n' "$?"
			fi
		else
			printf '%s\n' 'apk unavailable'
		fi
		if command -v opkg >/dev/null 2>&1; then
			if opkg status cake-adapt; then :; else printf 'opkg_status_rc=%s\n' "$?"; fi
			if opkg list-installed cake-adapt; then :; else
				printf 'opkg_list_rc=%s\n' "$?"
			fi
		else
			printf '%s\n' 'opkg unavailable'
		fi
	} > "$state_dir/package.txt" 2>&1

	{
		printf '%s\n' '=== relevant file metadata and hashes ==='
		for relevant_file in \
			/etc/config/cake-adapt \
			/etc/init.d/cake-adapt \
			/usr/sbin/cake-adapt \
			/lib/bpf/cake-adapt-tcpdelay.o; do
			if [ -e "$relevant_file" ]; then
				ls -ln "$relevant_file"
				sha256sum "$relevant_file"
			else
				printf 'absent %s\n' "$relevant_file"
			fi
		done
	} > "$state_dir/files.txt" 2>&1

	{
		printf '%s\n' '=== root namespaces and network state ==='
		ip -details rule show
		ip -4 route show table all
		ip -6 route show table all
		ip -s -d link show
		printf '%s\n' '=== root CAKE qdiscs ==='
		tc -s -d qdisc show
		printf '%s\n' '=== named network namespaces ==='
		namespace_list
		for netns_name in $(namespace_list); do
			printf '=== namespace %s: links ===\n' "$netns_name"
			ip -n "$netns_name" -s -d link show
			printf '=== namespace %s: rules and routes ===\n' "$netns_name"
			ip -n "$netns_name" -details rule show
			ip -n "$netns_name" -4 route show table all
			ip -n "$netns_name" -6 route show table all
			printf '=== namespace %s: CAKE qdiscs ===\n' "$netns_name"
			ip netns exec "$netns_name" tc -s -d qdisc show
		done
	} > "$state_dir/network.txt" 2>&1 || fail "network state capture failed in $state_dir"

	{
		printf '%s\n' '=== root namespaces and stable network state ==='
		ip -details rule show
		ip -4 route show table all
		ip -6 route show table all
		ip -o link show
		printf '%s\n' '=== root CAKE qdiscs ==='
		tc -d qdisc show
		printf '%s\n' '=== named network namespaces ==='
		namespace_list
		for netns_name in $(namespace_list); do
			printf '=== namespace %s: links ===\n' "$netns_name"
			ip -n "$netns_name" -o link show
			printf '=== namespace %s: rules and routes ===\n' "$netns_name"
			ip -n "$netns_name" -details rule show
			ip -n "$netns_name" -4 route show table all
			ip -n "$netns_name" -6 route show table all
			printf '=== namespace %s: CAKE qdiscs ===\n' "$netns_name"
			ip netns exec "$netns_name" tc -d qdisc show
		done
	} > "$state_dir/network.stable.txt" 2>&1 || fail "stable network capture failed in $state_dir"

	{
		printf '%s\n' '=== BPF statistics ==='
		if [ -r /proc/sys/kernel/bpf_stats_enabled ]; then
			cat /proc/sys/kernel/bpf_stats_enabled
		else
			printf '%s\n' 'kernel.bpf_stats_enabled unavailable'
		fi
		if command -v bpftool >/dev/null 2>&1; then
			bpftool prog show
			bpftool map show
		else
			printf '%s\n' 'bpftool unavailable'
		fi
	} > "$state_dir/bpf.txt" 2>&1
}

cleanup()
{
	cleanup_status=$?
	trap - EXIT HUP INT TERM
	if [ "$LOG_LINK_OWNED" -eq 1 ] && [ -n "$LOG_LINK" ] && [ -e "$LOG_LINK" ]; then
		if [ -n "$LOG_DEVICE_INODE" ] && [ "$(log_identity)" = "$LOG_DEVICE_INODE" ]; then
			cp "$TEST_LOG" "$RESULT_DIR/sqm-mon-test.log.capture" 2>/dev/null || true
			rm -f "$LOG_LINK"
		fi
	fi
	if [ -n "$LOG_DIR" ]; then
		rmdir "$LOG_DIR" 2>/dev/null || true
	fi
	if [ "$AFTER_CAPTURED" -eq 0 ] && [ -n "$RESULT_DIR" ] && [ -d "$RESULT_DIR" ]; then
		capture_state "$RESULT_DIR/after-failure" >/dev/null 2>&1 || true
		if [ "$NAMESPACE_RUN_STARTED" -eq 1 ]; then
			capture_namespaces > "$RESULT_DIR/namespaces.after-failure.txt" 2>/dev/null || true
			if cmp -s "$RESULT_DIR/namespaces.before.txt" \
				"$RESULT_DIR/namespaces.after-failure.txt"; then
				printf '%s\n' 'unchanged' > "$RESULT_DIR/namespaces.after-failure.diff"
			else
				printf '%s\n' 'changed' > "$RESULT_DIR/namespaces.after-failure.diff"
			fi
		fi
	fi
	if [ "$LOCK_OWNED" -eq 1 ]; then
		rmdir "$LOCK" 2>/dev/null || true
	fi
	exit "$cleanup_status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ "$(id -u)" -eq 0 ] || fail 'run this evidence harness as root on the OpenWrt VM'
for required_command in sed head tr readlink cat ls sha256sum ip tc date mkdir cp rm rmdir cmp pidof touch ln mktemp sort; do
	have "$required_command"
done
[ -x "$RUNNER" ] || fail "runner missing or not executable: $RUNNER"
[ -r "$OBJECT" ] || fail "BPF object missing or unreadable: $OBJECT"
[ -r "$WRAPPER" ] || fail "inner wrapper missing: $WRAPPER"
[ -x /etc/init.d/cake-adapt ] || :

mkdir "$LOCK" 2>/dev/null || fail "another evidence run may be active (lock: $LOCK)"
LOCK_OWNED=1

stamp=$(date -u '+%Y%m%dT%H%M%SZ')
RESULT_DIR=/root/cake-adapt-tcp-lifetime-capacity-$stamp-$$
mkdir "$RESULT_DIR" || fail "cannot create result directory: $RESULT_DIR"
printf '%s\n' "$RESULT_DIR" > "$RESULT_DIR/result-directory.txt"

{
	uname -a
	printf '\n=== versions ===\n'
	busybox 2>&1 || true
	ip -Version 2>&1 || true
	tc -V 2>&1 || true
	printf '%s\n' 'runner alarm bounds the BPF batch; outer SSH command has a 300 s timeout'
	opkg --version 2>&1 || true
	printf '\n=== available tools ===\n'
	for version_command in bpftool clang llvm-objdump; do
		if command -v "$version_command" >/dev/null 2>&1; then
			"$version_command" --version 2>&1 | head -n 3 || true
		else
			printf '%s unavailable\n' "$version_command"
		fi
		done
	printf '\n=== CPU availability ===\n'
	if [ -r /sys/devices/system/cpu/online ]; then
		printf 'online='; cat /sys/devices/system/cpu/online
	fi
	printf 'processor_count='; grep -c '^processor' /proc/cpuinfo
} > "$RESULT_DIR/platform.txt" 2>&1

capture_namespaces > "$RESULT_DIR/namespaces.before.txt"
capture_state "$RESULT_DIR/before"

if [ -e "$TEST_LOG" ]; then
	[ -f "$TEST_LOG" ] && [ ! -L "$TEST_LOG" ] || fail "$TEST_LOG must be a regular, non-symlink file"
	LOG_DEVICE_INODE=$(log_identity) || fail "cannot read inode for $TEST_LOG"
	check_log_writers "$LOG_DEVICE_INODE"
else
	touch "$TEST_LOG" || fail "cannot create $TEST_LOG"
	LOG_DEVICE_INODE=$(log_identity) || fail "cannot read inode for newly created $TEST_LOG"
fi

check_log_writers "$LOG_DEVICE_INODE"
LOG_DIR=$(mktemp -d /tmp/cake-adapt-tcp-lifetime-logs.XXXXXX) || fail 'cannot create isolated log directory on /tmp'
LOG_DIR_OWNED=1
LOG_LINK=$LOG_DIR/cake-adapt.log
ln "$TEST_LOG" "$LOG_LINK" || fail "cannot hard-link test log into isolated log directory"
LOG_LINK_OWNED=1

{
	printf 'runner='; sha256sum "$RUNNER"
	printf 'object='; sha256sum "$OBJECT"
	printf 'wrapper='; sha256sum "$WRAPPER"
	printf 'evidence='; sha256sum "$0"
	printf 'log_inode=%s\n' "$LOG_DEVICE_INODE"
	printf 'log_override=%s\n' "$LOG_DIR"
	printf 'namespace_baseline_file=%s\n' "$RESULT_DIR/namespaces.before.txt"
} > "$RESULT_DIR/input-checksums.txt"

check_log_writers "$LOG_DEVICE_INODE"
[ "$(log_identity)" = "$LOG_DEVICE_INODE" ] || fail "$TEST_LOG inode changed before truncation"
: > "$TEST_LOG"

RUN_RC=0
NAMESPACE_RUN_STARTED=1
if "$WRAPPER" "$RUNNER" "$OBJECT" \
	> "$RESULT_DIR/runner.stdout" 2> "$RESULT_DIR/runner.stderr"; then
	RUN_RC=0
else
	RUN_RC=$?
fi
printf '%s\n' "$RUN_RC" > "$RESULT_DIR/runner.exit-code"

capture_namespaces > "$RESULT_DIR/namespaces.after.txt"
if cmp -s "$RESULT_DIR/namespaces.before.txt" "$RESULT_DIR/namespaces.after.txt"; then
	printf '%s\n' 'unchanged' > "$RESULT_DIR/namespaces.diff"
else
	printf '%s\n' 'changed' > "$RESULT_DIR/namespaces.diff"
fi
capture_state "$RESULT_DIR/after"
AFTER_CAPTURED=1

if [ "$(log_identity)" = "$LOG_DEVICE_INODE" ]; then
	cp "$TEST_LOG" "$RESULT_DIR/sqm-mon-test.log.capture"
else
	printf '%s\n' 'ERROR: test log inode changed' > "$RESULT_DIR/log-inode.failure"
fi

{
	printf 'log_inode_before=%s\n' "$LOG_DEVICE_INODE"
	printf 'log_inode_after=%s\n' "$(log_identity 2>&1 || true)"
	if [ "$(log_identity)" = "$LOG_DEVICE_INODE" ]; then
		printf '%s\n' 'log_inode_unchanged=yes'
	else
		printf '%s\n' 'log_inode_unchanged=no'
	fi
	printf 'runner_exit_code=%s\n' "$RUN_RC"
} > "$RESULT_DIR/summary.txt"

state_match=1
for state_file in package.txt files.txt processes.stable.txt network.stable.txt; do
	if cmp -s "$RESULT_DIR/before/$state_file" "$RESULT_DIR/after/$state_file"; then
		printf '%s unchanged\n' "$state_file" >> "$RESULT_DIR/state-comparison.txt"
	else
		printf '%s CHANGED\n' "$state_file" >> "$RESULT_DIR/state-comparison.txt"
		state_match=0
	fi
done
if cmp -s "$RESULT_DIR/namespaces.before.txt" "$RESULT_DIR/namespaces.after.txt"; then
	printf '%s unchanged\n' 'network namespace list' >> "$RESULT_DIR/state-comparison.txt"
else
	printf '%s CHANGED (outer script leaves namespace cleanup to the wrapper)\n' \
		'network namespace list' >> "$RESULT_DIR/state-comparison.txt"
	state_match=0
fi

if [ "$(log_identity)" != "$LOG_DEVICE_INODE" ]; then
	fail "$TEST_LOG inode changed; evidence retained at $RESULT_DIR"
fi
if [ "$state_match" -ne 1 ]; then
	fail "VM state differs from baseline; inspect $RESULT_DIR/state-comparison.txt"
fi
if [ "$RUN_RC" -ne 0 ]; then
	fail "inner harness returned $RUN_RC; evidence retained at $RESULT_DIR"
fi

printf 'Evidence captured in %s\n' "$RESULT_DIR"
exit 0
