#!/bin/sh
# Usage: sh bpfcost.sh SECONDS   ns per run of the daemon's BPF programs over that window
# Reads the counters from the daemon's program descriptors in /proc, so it
# needs no bpftool (bpftool-full printed nothing on the router; -minimal works).
W=${1:-60}
snap() {
	P=$(pidof cake-adapt) || return
	for f in /proc/$P/fd/*; do
		[ "$(readlink $f)" = anon_inode:bpf-prog ] || continue
		awk '/^prog_type/{t=$2} /^run_time_ns/{n=$2} /^run_cnt/{c=$2}
		     END{print (t==1?"filter":t==3?"injector":"type" t), n, c}' /proc/$P/fdinfo/${f##*/}
	done
}
sysctl -qw kernel.bpf_stats_enabled=1
snap > /tmp/bpf.before; sleep "$W"; snap > /tmp/bpf.after
sysctl -qw kernel.bpf_stats_enabled=0
awk 'NR==FNR{t[$1]=$2;c[$1]=$3;next} {dc=$3-c[$1]; if(dc>0) printf "%-9s %6.0f ns/run  %d runs\n",$1,($2-t[$1])/dc,dc}' /tmp/bpf.before /tmp/bpf.after
