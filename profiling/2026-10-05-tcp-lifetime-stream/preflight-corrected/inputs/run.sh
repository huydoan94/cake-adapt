#!/bin/sh
set -eu

mode=
if [ "$#" -eq 3 ] && [ "$1" = "--preflight" ]; then
	mode=--preflight
	shift
elif [ "$#" -ne 2 ]; then
	printf 'usage: %s [--preflight] OBJECT RESULT_DIR\n' "$0" >&2
	exit 2
fi
object_arg=$1
result_dir=$2
case "$0" in
	*/*) runner_dir=${0%/*} ;;
	*) runner_dir=. ;;
esac
runner=
test_log=/tmp/sqm-mon-test.log
namespace=caketl$$
log_dir=/tmp/cake-adapt-tcp-lifetime-log.XXXXXX.$$
log_dir_owned=0
manifest_file=
manifest_ready=0
namespace_owned=0
log_link_owned=0
runner_pid=
runner_pid_file=/tmp/cake-adapt-tcp-lifetime-runner.$$.pid
deadline_marker=/tmp/cake-adapt-tcp-lifetime-deadline.$$
watchdog_pid=
hard_deadline_pid=
before_inode=
before_snapshot_done=0
after_snapshot_done=0
result_dir_ready=0
cleanup_running=0
overall_timeout_seconds=160
wrapper_pid=$$

manifest_record()
{
	[ "$manifest_ready" -eq 1 ] || return 1
	printf '%s\t%s\n' "$1" "$2" >> "$manifest_file" || return 1
}

snapshot()
{
	phase=$1
	snapshot_rc=0
	{
		uname -a || snapshot_rc=1
		if [ -r /etc/openwrt_release ]; then
			cat /etc/openwrt_release || snapshot_rc=1
		else
			printf '/etc/openwrt_release unavailable\n'
			snapshot_rc=1
		fi
		if command -v apk >/dev/null 2>&1; then
			apk info -a cake-adapt 2>&1 || snapshot_rc=1
		else
			printf 'apk unavailable\n'
		fi
	} > "$result_dir/$phase.package" || snapshot_rc=1
	{
		pidof cake-adapt 2>/dev/null || true
		pidof fping fping6 fping-ts irtt 2>/dev/null || true
		/etc/init.d/cake-adapt status 2>&1 || true
	} > "$result_dir/$phase.process" || snapshot_rc=1
	{
		for setting in \
			net/core/bpf_jit_enable \
			net/core/bpf_jit_harden \
			net/core/bpf_jit_kallsyms \
			kernel/unprivileged_bpf_disabled; do
			if [ -r "/proc/sys/$setting" ]; then
				printf '%s=' "$setting"
				cat "/proc/sys/$setting" || printf 'unavailable\n'
			else
				printf '%s=unavailable\n' "$setting"
			fi
		done
	} > "$result_dir/$phase.bpf-jit" || snapshot_rc=1
	{
		ip -o link show || snapshot_rc=1
		ip rule show || snapshot_rc=1
		ip -4 route show table all || snapshot_rc=1
		ip -6 route show table all || snapshot_rc=1
	} > "$result_dir/$phase.network" || snapshot_rc=1
	tc -d qdisc show > "$result_dir/$phase.qdisc" || snapshot_rc=1
	ip netns list > "$result_dir/$phase.namespaces" || snapshot_rc=1
	for file in /etc/config/cake-adapt /etc/init.d/cake-adapt /usr/sbin/cake-adapt /lib/bpf/cake-adapt-tcpdelay.o; do
		if [ -e "$file" ]; then
			sha256sum "$file" || snapshot_rc=1
		else
			printf 'missing %s\n' "$file"
		fi
	done > "$result_dir/$phase.files" || snapshot_rc=1
	return "$snapshot_rc"
}

compare_snapshots()
{
	state_rc=0
	for state in package process bpf-jit network qdisc files namespaces; do
		if cmp -s "$result_dir/before.$state" "$result_dir/after.$state"; then
			printf '%s unchanged\n' "$state"
		else
			printf '%s CHANGED\n' "$state"
			state_rc=1
		fi
	done
	return "$state_rc"
}

cleanup()
{
	status=$?
	[ "$cleanup_running" -eq 0 ] || return
	cleanup_running=1
	trap - EXIT HUP INT TERM
	set +e
	if [ -n "$runner_pid" ]; then
		if kill -0 "$runner_pid" 2>/dev/null; then
			kill -TERM "$runner_pid" 2>/dev/null
			attempt=0
			while kill -0 "$runner_pid" 2>/dev/null && [ "$attempt" -lt 50 ]; do
				sleep 0.1
				attempt=$((attempt + 1))
			done
			if kill -0 "$runner_pid" 2>/dev/null; then
				kill -KILL "$runner_pid" 2>/dev/null
			fi
		fi
		wait "$runner_pid" 2>/dev/null
	fi
	runner_pid=
	rm -f "$runner_pid_file"
	if [ "$namespace_owned" -eq 1 ]; then
		manifest_record namespace "delete-intent $namespace" || status=1
		ip netns del "$namespace" || status=1
		namespace_owned=0
		manifest_record namespace "deleted $namespace" || status=1
	fi
	if [ -n "$result_dir" ] && [ "$result_dir_ready" -eq 1 ] &&
	   [ "$before_snapshot_done" -eq 1 ] && [ "$after_snapshot_done" -eq 0 ]; then
		if snapshot after; then
			after_snapshot_done=1
		else
			status=1
		fi
	fi
	if [ "$log_link_owned" -eq 1 ]; then
		manifest_record log-link "remove-intent $log_dir/cake-adapt.log" || status=1
		rm -f "$log_dir/cake-adapt.log" || status=1
		log_link_owned=0
		manifest_record log-link removed || status=1
	fi
	if [ "$log_dir_owned" -eq 1 ]; then
		manifest_record log-directory "remove-intent $log_dir" || status=1
		rmdir "$log_dir" || status=1
		log_dir_owned=0
		manifest_record log-directory removed || status=1
	fi
	if [ -n "$before_inode" ]; then
		if [ -e "$test_log" ]; then
			after_inode=$("$runner" --file-identity "$test_log")
			printf '%s\n' "$after_inode" > "$result_dir/log-inode.after"
			if [ "$before_inode" != "$after_inode" ]; then
				printf 'test log device/inode changed\n' >&2
				status=1
			fi
		else
			printf 'recorded test log disappeared: %s\n' "$test_log" >&2
			status=1
		fi
	fi
	if [ "$namespace_owned" -eq 0 ] && ip netns list | awk '{print $1}' | grep -Fx "$namespace" >/dev/null 2>&1; then
		printf 'test namespace remains: %s\n' "$namespace" >&2
		status=1
	fi
	if [ "$before_snapshot_done" -eq 1 ] && [ "$after_snapshot_done" -eq 1 ]; then
		compare_snapshots || status=1
	fi
	if [ -n "$watchdog_pid" ]; then
		kill -TERM "$watchdog_pid" 2>/dev/null
		wait "$watchdog_pid" 2>/dev/null
	fi
	if [ -n "$hard_deadline_pid" ]; then
		kill -TERM "$hard_deadline_pid" 2>/dev/null
		wait "$hard_deadline_pid" 2>/dev/null
	fi
	rm -f "$deadline_marker"
	exit "$status"
}

# Install cleanup before creating result files, touching the test log, or
# creating target namespace state.
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

rm -f "$deadline_marker" "$runner_pid_file"
(
	sleep "$((overall_timeout_seconds - 10))" &
	timer_sleep=$!
	trap 'kill "$timer_sleep" 2>/dev/null; wait "$timer_sleep" 2>/dev/null; exit 0' TERM INT HUP
	wait "$timer_sleep"
	: > "$deadline_marker"
	kill -TERM "$wrapper_pid" 2>/dev/null
) &
watchdog_pid=$!
(
	sleep "$overall_timeout_seconds" &
	timer_sleep=$!
	trap 'kill "$timer_sleep" 2>/dev/null; wait "$timer_sleep" 2>/dev/null; exit 0' TERM INT HUP
	wait "$timer_sleep"
	if [ -e "$deadline_marker" ]; then
		if [ -r "$runner_pid_file" ]; then
			runner_target=$(cat "$runner_pid_file")
			kill -KILL "$runner_target" 2>/dev/null
		fi
		kill -KILL "$wrapper_pid" 2>/dev/null
	fi
) &
hard_deadline_pid=$!

runner_dir=$(CDPATH= cd -- "$runner_dir" && pwd)
runner=$runner_dir/../../build/tcp-lifetime/runner
object=$(readlink -f "$object_arg")
mkdir "$result_dir"
result_dir=$(readlink -f "$result_dir")
result_dir_ready=1
manifest_file=$result_dir/ownership.manifest
: > "$manifest_file"
manifest_ready=1
manifest_record wrapper-pid "$wrapper_pid"
manifest_record namespace "planned $namespace"
manifest_record log-directory "planned $log_dir"
manifest_record runner-pid-file "$runner_pid_file"
manifest_record soft-deadline-path "$deadline_marker"
manifest_record soft-watchdog-pid "$watchdog_pid"
manifest_record hard-watchdog-pid "$hard_deadline_pid"
manifest_record runner "planned $runner"
if [ ! -x "$runner" ] || [ ! -r "$object" ]; then
	printf 'runner or object is unavailable\n' >&2
	exit 1
fi
snapshot before
before_snapshot_done=1
if ip netns list | awk '{print $1}' | grep -Fx "$namespace" >/dev/null 2>&1; then
	printf 'namespace name collision: %s\n' "$namespace" >&2
	exit 1
fi
if [ -e "$log_dir" ] || [ -L "$log_dir" ]; then
	printf 'isolated log directory already exists: %s\n' "$log_dir" >&2
	exit 1
fi

if [ -e "$test_log" ]; then
	log_identity=$("$runner" --file-identity "$test_log")
	for descriptor in /proc/[0-9]*/fd/*; do
		[ -e "$descriptor" ] || continue
		fd_identity=$("$runner" --file-identity "$descriptor" 2>/dev/null || true)
		[ "$fd_identity" = "$log_identity" ] || continue
		fdinfo=${descriptor%/fd/*}/fdinfo/${descriptor##*/}
		flags=$(sed -n 's/^flags:[[:space:]]*//p' "$fdinfo" 2>/dev/null | head -n 1)
		case "$flags" in
			''|*[!0-7]*)
				printf 'cannot read test-log access flags: %s\n' "$descriptor" >&2
				exit 1
				;;
		esac
		access_mode=$((flags & 3))
		case "$access_mode" in
			1|2)
				printf 'test log has a writer: %s\n' "$descriptor" >&2
				exit 1
				;;
			3)
				printf 'invalid test-log access mode: %s\n' "$descriptor" >&2
				exit 1
				;;
		esac
	done
fi
if [ ! -e "$test_log" ]; then
	manifest_record test-log "create-intent $test_log"
	touch "$test_log"
	manifest_record test-log "created $test_log"
fi
if [ -L "$test_log" ] || [ ! -f "$test_log" ]; then
	printf 'test log is not a regular file\n' >&2
	exit 1
fi
before_inode=$("$runner" --file-identity "$test_log")
printf '%s\n' "$before_inode" > "$result_dir/log-inode.before"
manifest_record test-log "inode $before_inode path $test_log"
manifest_record test-log truncate-intent
: > "$test_log"
manifest_record test-log truncated
manifest_record log-directory "mkdir-intent $log_dir"
mkdir "$log_dir"
log_dir_owned=1
manifest_record log-directory "created $log_dir"
manifest_record log-link "create-intent $log_dir/cake-adapt.log"
ln "$test_log" "$log_dir/cake-adapt.log"
log_link_owned=1
manifest_record log-link "created $log_dir/cake-adapt.log"
log_file_path_override=$log_dir
export log_file_path_override
sha256sum "$object" "$runner" "$runner_dir/runner.c" "$runner_dir/log-shim.c" \
	"$runner_dir/Makefile" "$runner_dir/run.sh" \
	"$runner_dir/../../src/tcpdelay/tcpdelay.bpf.c" \
	"$runner_dir/../../src/tcpdelay/capture.c" \
	"$runner_dir/../../src/tcpdelay/estimator.c" \
	"$runner_dir/../../src/tcpdelay/lifetime.c" \
	"$runner_dir/../../src/tcpdelay/record.h" \
	"$runner_dir/../../src/tcpdelay/capture.h" \
	"$runner_dir/../../src/tcpdelay/estimator.h" \
	"$runner_dir/../../src/tcpdelay/lifetime.h" \
	"$runner_dir/../../src/common/error.c" \
	"$runner_dir/../../src/common/error.h" \
	"$runner_dir/../../src/common/helpers.c" \
	"$runner_dir/../../src/common/helpers.h" \
	"$runner_dir/../../src/common/constants.h" \
	"$runner_dir/../../src/common/utils.h" \
	"$runner_dir/../../src/logging/log.h" \
	"$runner_dir/../../src/cake/accounting.h" \
	> "$result_dir/input-checksums"

manifest_record namespace "create-intent $namespace"
ip netns add "$namespace"
namespace_owned=1
manifest_record namespace "created $namespace"
manifest_record veth "create-intent namespace=$namespace names=tl0,tl1"
ip -n "$namespace" link add tl0 type veth peer name tl1
manifest_record veth "created namespace=$namespace names=tl0,tl1"
ip -n "$namespace" link set tl0 address 02:00:00:00:00:01
ip -n "$namespace" link set tl1 address 02:00:00:00:00:02
ip -n "$namespace" link set tl0 up
ip -n "$namespace" link set tl1 up
run_rc=0
manifest_record runner launch-intent
if [ -n "$mode" ]; then
	ip netns exec "$namespace" "$runner" "$mode" "$object" tl0 tl1 \
		> "$result_dir/runner.stdout" 2> "$result_dir/runner.stderr" &
else
	ip netns exec "$namespace" "$runner" "$object" tl0 tl1 \
		> "$result_dir/runner.stdout" 2> "$result_dir/runner.stderr" &
fi
runner_pid=$!
printf '%s\n' "$runner_pid" > "$runner_pid_file"
manifest_record runner-pid "$runner_pid"
wait "$runner_pid" || run_rc=$?
runner_pid=
rm -f "$runner_pid_file"
printf '%s\n' "$run_rc" > "$result_dir/runner.exit-code"
manifest_record namespace "delete-intent $namespace"
ip netns del "$namespace"
namespace_owned=0
manifest_record namespace "deleted $namespace"
snapshot after
after_snapshot_done=1
compare_snapshots || state_rc=1
state_rc=${state_rc:-0}
printf 'RESULT_DIR=%s RUN_RC=%s STATE_RC=%s\n' "$result_dir" "$run_rc" "$state_rc"
if [ "$run_rc" -ne 0 ]; then exit "$run_rc"; fi
exit "$state_rc"
