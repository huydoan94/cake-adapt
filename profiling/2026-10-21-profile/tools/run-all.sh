#!/bin/sh
# run-all.sh: the 0.3.21 profile on the x86 VM (2026-10-21). Three variants
# (v0321-native, v0321-fping, v0315), three CPU passes each in a rotating
# order, then one perf and one strace pass each. Every run is moved to
# /root/prof as soon as it ends, so a VM panic loses only the run in progress,
# and a rerun skips the runs already kept. The installed service is stopped
# meanwhile and started again at the end.
set -u
DIR=/tmp/e2e
OUT=/root/prof
mkdir -p "$OUT" "$DIR/prof"
/etc/init.d/cake-adapt stop; sleep 2
n=0
for step in "v0321-native cpu" "v0321-fping cpu" "v0315 cpu" \
	"v0315 cpu" "v0321-native cpu" "v0321-fping cpu" \
	"v0321-fping cpu" "v0315 cpu" "v0321-native cpu" \
	"v0321-native perf" "v0321-fping perf" "v0315 perf" \
	"v0321-native strace" "v0321-fping strace" "v0315 strace"; do
	set -- $step
	n=$((n + 1)); run=$(printf '%02d-%s-%s' "$n" "$1" "$2")
	[ -d "$OUT/$run" ] && continue
	echo "$(date +%T) start $run" >> "$OUT/progress"
	sh "$DIR/prof.sh" "$1" "$2" "$run" > "$DIR/prof/$run.out" 2>&1
	mv "$DIR/prof/$run.out" "$DIR/prof/$run/" 2>/dev/null
	mv "$DIR/prof/$run" "$OUT/$run"
	echo "$(date +%T) done $run" >> "$OUT/progress"
done
/etc/init.d/cake-adapt start
echo "$(date +%T) finished" >> "$OUT/progress"
