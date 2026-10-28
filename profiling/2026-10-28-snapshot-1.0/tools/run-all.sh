#!/bin/sh
# run-all.sh: 1.0.0 (v100) against 0.3.30 (v0330)
# on the snapshot x86_64 VM .2 (2026-10-28). Three CPU passes each in a rotating order,
# then one perf and one strace pass of v100. Each run is moved to
# /root/prof-snap as soon as it ends.
set -u
DIR=/tmp/e2e
OUT=/root/prof-snap
mkdir -p "$OUT" "$DIR/prof"
/etc/init.d/cake-adapt stop; sleep 2
n=0
for step in "v100-native cpu" "v0330-native cpu" "v0330-native cpu" \
	"v100-native cpu" "v100-native cpu" "v0330-native cpu" \
	"v100-native perf" "v100-native strace"; do
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
