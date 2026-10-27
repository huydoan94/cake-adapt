#!/bin/sh
# make-graphs.sh RAW OUT: folded stacks and flame graphs from each perf pass in
# RAW (RAW/NN-VARIANT-perf/WORKLOAD.perf-script.txt.gz), with FlameGraph 41fee1f
# (stackcollapse-perf.pl --kernel, as on 2026-10-09) from build/FlameGraph.
# Writes OUT/folded/VARIANT-WORKLOAD.folded and OUT/flamegraphs/VARIANT-WORKLOAD.svg.
set -eu
raw=$1 out=$2
here=$(cd "$(dirname "$0")" && pwd)
flamegraph=${FLAMEGRAPH:-$here/../../../build/FlameGraph}
mkdir -p "$out/folded" "$out/flamegraphs"
for run in "$raw"/*-perf; do
	variant=$(cut -d' ' -f1 "$run/variant")
	for script in "$run"/*.perf-script.txt.gz; do
		workload=$(basename "$script" .perf-script.txt.gz)
		folded=$out/folded/$variant-$workload.folded
		gzip -dc "$script" | perl "$flamegraph/stackcollapse-perf.pl" --kernel > "$folded"
		perl "$flamegraph/flamegraph.pl" --title "cake-adapt $variant, $workload (cpu-clock 999 Hz, 40 s)" \
			--colors java "$folded" > "$out/flamegraphs/$variant-$workload.svg"
	done
done
