#!/bin/sh
# run-all.sh: the 0.3.15 (head) and 0.3.12 (base) profile on the x86 VM.
# Three alternated CPU passes per build, then one perf and one strace pass
# each. Every run is moved to /root/prof as soon as it ends, so a VM panic
# loses only the run in progress. The installed service is stopped meanwhile
# and started again at the end.
set -u
DIR=/tmp/e2e
OUT=/root/prof
mkdir -p "$OUT"
/etc/init.d/cake-adapt stop; sleep 2
n=0
for step in "head cpu" "base cpu" "base cpu" "head cpu" "head cpu" "base cpu" \
	"head perf" "base perf" "head strace" "base strace"; do
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
