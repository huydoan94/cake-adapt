#!/bin/sh
# run-all.sh: the restructured tree (vrestr) against 0.3.26 before it (v0326)
# on the x86 VM clone (2026-10-22). Three CPU passes each in a rotating order,
# then one perf and one strace pass of vrestr. Each run is moved to
# /root/prof-restr as soon as it ends.
set -u
DIR=/tmp/e2e
OUT=/root/prof-restr
mkdir -p "$OUT" "$DIR/prof"
/etc/init.d/cake-adapt stop; sleep 2
n=0
for step in "vrestr-native cpu" "v0326-native cpu" "v0326-native cpu" \
	"vrestr-native cpu" "vrestr-native cpu" "v0326-native cpu" \
	"vrestr-native perf" "vrestr-native strace"; do
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
