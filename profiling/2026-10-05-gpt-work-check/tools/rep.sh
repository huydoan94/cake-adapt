#!/bin/sh
# One repetition: $1 = mode name, rest = variant specs. Copies results to /root afterwards.
mode=$1; shift
timeout 900 ssh -o ConnectTimeout=5 -o BatchMode=yes root@192.168.56.2 "rm -rf /tmp/eval && cp -a /root/eval /tmp/eval && cd /tmp/eval && LD_LIBRARY_PATH=/tmp/eval/lib ./sample --limit 780 ./sample --pin sh ./run-matrix.sh /tmp/eval $mode $*; status=\$?; echo exit \$status; pidof cake-adapt"
