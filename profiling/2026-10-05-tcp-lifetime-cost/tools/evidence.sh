#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
	printf 'usage: %s RUNNER BASELINE_OBJECT CANDIDATE_OBJECT short|repeat\n' "$0" >&2
	exit 2
fi

runner=$1
baseline=$2
candidate=$3
mode=$4
lock=/tmp/cake-adapt-tcp-lifetime-cost.lock
result_dir="/root/cake-adapt-tcp-lifetime-cost-$(date -u +%Y%m%dT%H%M%SZ)-$$"
state_captured=0

[ -x "$runner" ] && [ -r "$baseline" ] && [ -r "$candidate" ] || {
	printf 'runner or object unavailable\n' >&2
	exit 1
}
mkdir "$lock" 2>/dev/null || { printf 'cost test lock exists\n' >&2; exit 1; }
mkdir "$result_dir"
cleanup()
{
	run_status=$?
	trap - EXIT
	if [ "$state_captured" -eq 1 ]; then
		set +e
		capture_state "$result_dir/state.after"
		if cmp -s "$result_dir/state.before" "$result_dir/state.after"; then
			printf 'unchanged\n' > "$result_dir/state-comparison.txt"
		else
			diff -u "$result_dir/state.before" "$result_dir/state.after" > "$result_dir/state.diff"
			printf 'changed\n' > "$result_dir/state-comparison.txt"
			run_status=1
		fi
		set -e
	fi
	rmdir "$lock" 2>/dev/null || true
	exit "$run_status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

capture_state()
{
	destination=$1
	{
		uname -a
		printf 'online='; cat /sys/devices/system/cpu/online
		printf 'daemon_pid='; pidof cake-adapt || true
		printf 'package='; opkg status cake-adapt 2>/dev/null | sed -n '1,8p' || true
		printf 'files\n'
		for file in /etc/config/cake-adapt /etc/init.d/cake-adapt /usr/sbin/cake-adapt /lib/bpf/cake-adapt-tcpdelay.o; do
			if [ -e "$file" ]; then ls -ln "$file"; sha256sum "$file"; else printf 'absent %s\n' "$file"; fi
		done
		printf 'processes\n'
		for proc_dir in /proc/[0-9]*; do
			[ -r "$proc_dir/comm" ] || continue
			[ "$(cat "$proc_dir/comm" 2>/dev/null || true)" = cake-adapt ] || continue
			printf 'pid=%s exe=' "${proc_dir#/proc/}"
			readlink "$proc_dir/exe" 2>/dev/null || true
			printf '\n'
		done
		printf 'namespaces\n'; ip netns list | sed 's/[[:space:]].*$//' | sort
		printf 'root network\n'; ip -details rule show; ip -o link show
		ip -4 route show table all; ip -6 route show table all
		printf 'root qdiscs\n'; tc -d qdisc show
		printf 'test log identity\n'; ls -id /tmp/sqm-mon-test.log 2>&1 || true
	} > "$destination" 2>&1
}

capture_state "$result_dir/state.before"
state_captured=1
printf 'runner '; sha256sum "$runner" > "$result_dir/inputs.sha256"
printf 'baseline '; sha256sum "$baseline" >> "$result_dir/inputs.sha256"
printf 'candidate '; sha256sum "$candidate" >> "$result_dir/inputs.sha256"
case "$mode" in
	short)
		printf 'short_pair=baseline,candidate\n' > "$result_dir/order.txt"
		"$(dirname "$0")/run.sh" "$runner" "$baseline" short-baseline > "$result_dir/short-baseline.txt"
		"$(dirname "$0")/run.sh" "$runner" "$candidate" short-candidate > "$result_dir/short-candidate.txt"
		;;
	repeat)
		printf 'pair_1=baseline,candidate\npair_2=baseline,candidate\npair_3=baseline,candidate\n' > "$result_dir/order.txt"
		for item in 1 2 3; do
			"$(dirname "$0")/run.sh" "$runner" "$baseline" "baseline-$item" > "$result_dir/baseline-$item.txt"
			"$(dirname "$0")/run.sh" "$runner" "$candidate" "candidate-$item" > "$result_dir/candidate-$item.txt"
		done
		;;
	*)
		printf 'mode must be short or repeat\n' >&2
		exit 2
		;;
esac
printf 'Evidence directory: %s\n' "$result_dir"
