#!/bin/bash
# run.sh VM OUTDIR [CAPTURE]: the follower profile, run from the PC. Three minutes with
# no follower, three with capture-log.sh -l following the log, then one
# minute each of strace -f and strace -c on the follower; the samples and
# traces are copied to OUTDIR. CAPTURE is the capture-log.sh to use
# (default: the repository's).
set -u
vm=$1 out=$2
repo=$(cd "$(dirname "$0")/../../.." && pwd)
capture_script=${3:-$repo/tools/capture-log.sh}
mkdir -p "$out"
scp -O -q "$repo/profiling/2026-10-09-follower/tools/fprof.sh" "$vm:/tmp/fprof.sh"
scp -O -q "$repo/profiling/2026-10-09-profile/tools/cake-adapt.template" "$vm:/tmp/fprof-template"
scp -O -q "$repo/tools/testbed/testbed.sh" "$vm:/tmp/e2e/testbed.sh"
ssh "$vm" 'rm -rf /root/fprof; sh /tmp/fprof.sh up'
sleep 30
ssh "$vm" 'sh /tmp/fprof.sh sample none 180'
bash "$capture_script" -l "$vm" "$out/captured.log" /tmp/fprof/logs/cake-adapt.log > /dev/null 2>&1 &
capture=$!
sleep 30
ssh "$vm" 'sh /tmp/fprof.sh sample follower 180'
ssh "$vm" 'sh /tmp/fprof.sh strace 60'
kill -INT "$capture" 2>/dev/null; kill "$capture" 2>/dev/null
wait "$capture" 2>/dev/null
sleep 35
ssh "$vm" 'sh /tmp/fprof.sh sample after-capture 30; sh /tmp/fprof.sh down'
ssh "$vm" 'tar czf - -C /root fprof' | tar xzf - -C "$out"
echo done > "$out/done"
