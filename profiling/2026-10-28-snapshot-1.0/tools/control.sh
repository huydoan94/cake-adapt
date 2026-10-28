#!/bin/sh
# control.sh: 0.3.30 against 1.0.0 (v100) on the snapshot x86_64 VM .2 under load on the
# emulated ISP (x86 VM, 2026-10-28): three rounds, alternating which runs first,
# of tools/testbed's run.sh with the native pinger and cake-autorate's delay
# thresholds. Each run is moved to /root/e2e-control-snap as soon as it ends; the installed service is
# stopped meanwhile and started again at the end.
set -u
T=/tmp/cake-adapt-test
OUT=/root/e2e-control-snap
mkdir -p "$OUT"
/etc/init.d/cake-adapt stop
sleep 2
sh "$T/testbed.sh" up
for round in 1 2 3; do
	if [ $((round % 2)) -eq 1 ]; then order='v100 v0330'; else order='v0330 v100'; fi
	for variant in $order; do
		name=r$round-$variant
		[ -d "$OUT/$name" ] && continue
		echo "$(date +%T) start $name" >> "$OUT/progress"
		sh "$T/run.sh" "$name" "/tmp/e2e/bin/$variant" native 10 30 60 > "$T/$name.out" 2>&1
		mv "$T/results/$name" "$OUT/$name"
		mv "$T/$name.out" "$OUT/$name/run.out"
		echo "$(date +%T) done $name" >> "$OUT/progress"
	done
done
sh "$T/testbed.sh" down
/etc/init.d/cake-adapt start
echo "$(date +%T) finished" >> "$OUT/progress"
