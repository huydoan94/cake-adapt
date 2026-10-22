#!/bin/sh
# run-fix.sh: the log writer fix against 0.3.21 and 0.3.15 on the x86 VM
# (2026-10-22). Three CPU passes each in a rotating order, then one perf and
# one strace pass of the fix. Runs are kept in /root/prof-fix as in run-all.sh.
set -u
DIR=/tmp/e2e
OUT=/root/prof-fix
mkdir -p "$OUT" "$DIR/prof"
/etc/init.d/cake-adapt stop; sleep 2
n=0
for step in "vfix-native cpu" "v0321-native cpu" "v0315 cpu" \
	"v0315 cpu" "vfix-native cpu" "v0321-native cpu" \
	"v0321-native cpu" "v0315 cpu" "vfix-native cpu" \
	"vfix-native perf" "vfix-native strace"; do
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
