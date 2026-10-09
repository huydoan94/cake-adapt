#!/bin/sh
# cake-adapt lifecycle checks on the x86 VM. Evidence goes to $OUT.
OUT=/root/e2e/lifecycle; rm -rf $OUT; mkdir -p $OUT
LOG=/var/log/cake-adapt.log
B=/usr/sbin/cake-adapt
pass=0; fail=0
ok()   { echo "PASS $*" | tee -a $OUT/summary; pass=$((pass+1)); }
bad()  { echo "FAIL $*" | tee -a $OUT/summary; fail=$((fail+1)); }
check(){ name=$1; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }
pid()  { pidof cake-adapt; }
fpings(){ pidof fping | wc -w; }
links(){ bpftool net show dev eth1 | grep -c "tcx/egress inject_egress"; }
degraded(){ ! logread -e cake-adapt | tail -n 20 | grep -q "TCP measurement degraded"; }
mark() { echo "=== $(date +%s) $*" >> $OUT/timeline; logread -e cake-adapt | tail -1 >> $OUT/timeline; }
wait_log() { # pattern seconds
    i=0; while [ $i -lt $2 ]; do grep -q "$1" $LOG && return 0; sleep 1; i=$((i+1)); done; return 1; }
tc -d qdisc show > $OUT/qdiscs-original
grep -c ^processor /proc/cpuinfo > $OUT/cpus; free > $OUT/memory
uci show cake-adapt > $OUT/uci-original
cp /etc/config/cake-adapt $OUT/cake-adapt.config-original

# 1. CLI: listing and validation
$B -L > $OUT/options; check "L lists options" grep -qx ul_congest_ack_share $OUT/options
$B -V -C /tmp/cake-adapt-config/main -S main > $OUT/validate 2>&1; check "V accepts the running config" [ $? = 0 ]
mkdir -p /tmp/e2e-bad; sed 's/min_dl_shaper_rate_kbps .*/min_dl_shaper_rate_kbps '"'"'90000'"'"'/' /etc/config/cake-adapt > /tmp/e2e-bad/cake-adapt
$B -V -C /tmp/e2e-bad -S main > $OUT/validate-bad 2>&1; check "V rejects min > base" [ $? != 0 ]
grep -q "minimum <= base <= maximum" $OUT/validate-bad || bad "V names the rate rule"
sed 's/min_dl_shaper_rate_kbps .*/min_dl_shaper_rate_kbps '"'"'10001'"'"'/' /etc/config/cake-adapt > /tmp/e2e-bad/cake-adapt
$B -V -C /tmp/e2e-bad -S main > $OUT/validate-step 2>&1; check "V accepts 10001 kbit/s (whole bytes/s)" [ $? = 0 ]
sed 's/min_dl_shaper_rate_kbps .*/min_dl_shaper_rate_kbps '"'"'10000.001'"'"'/' /etc/config/cake-adapt > /tmp/e2e-bad/cake-adapt
$B -V -C /tmp/e2e-bad -S main > $OUT/validate-step2 2>&1; check "V rejects 10000.001 kbit/s (not whole bytes/s)" [ $? != 0 ]
rm -rf /tmp/e2e-bad

# 2. Service stop/start/restart, no leftover children
/etc/init.d/cake-adapt stop; sleep 3; mark stop
check "stop leaves no daemon" [ -z "$(pid)" ]
check "stop leaves no fping" [ "$(fpings)" = 0 ]
check "stop detaches the injector" [ "$(links)" = 0 ]
logread -e cake-adapt | tail -3 > $OUT/stop-syslog; check "stop is in syslog" grep -q "Stopped cake-adapt" $OUT/stop-syslog
inode=$(ls -i $LOG | awk '{print $1}')
/etc/init.d/cake-adapt start; sleep 6; mark start
check "start runs daemon" [ -n "$(pid)" ]
check "start runs one fping" [ "$(fpings)" = 1 ]
check "start attaches one injector" [ "$(links)" = 1 ]
check "start has TCP measurement" degraded
p1=$(pid); /etc/init.d/cake-adapt restart; sleep 6; p2=$(pid); mark restart
check "restart replaces the process" [ -n "$p2" ] && [ "$p1" != "$p2" ]
check "restart leaves one fping" [ "$(fpings)" = 1 ]
check "restart leaves one injector" [ "$(links)" = 1 ]

# 3. procd respawn after a crash
kill -9 $(pid); sleep 8; mark kill9
check "procd respawns after SIGKILL" [ -n "$(pid)" ]
sleep 2; check "one fping after respawn" [ "$(fpings)" = 1 ]
check "one injector after respawn" [ "$(links)" = 1 ]

# 4. CAKE disappears and returns (download IFB)
sleep 5
dl_line=$(tc -d qdisc show dev ifb4eth1 | grep '^qdisc cake')
echo "$dl_line" > $OUT/ifb-cake-before
: > $OUT/marker; before=$(wc -l < $LOG)
tc qdisc del dev ifb4eth1 root; mark del-ifb-cake
sleep 5
tail -n +$before $LOG > $OUT/after-delete.log
check "CAKE removal is reported" grep -q "CAKE removed" $OUT/after-delete.log
check "pingers stop without CAKE" [ "$(fpings)" = 0 ]
# Recreate exactly: same bandwidth and options as recorded.
opts=$(echo "$dl_line" | sed 's/^qdisc cake [0-9a-f]*: root refcnt [0-9]* //; s/ memlimit [^ ]*//')
tc qdisc add dev ifb4eth1 root cake $opts; mark add-ifb-cake
echo "$opts" > $OUT/ifb-cake-recreated-options
sleep 8
tail -n +$before $LOG > $OUT/after-readd.log
check "CAKE return is reported" grep -qE "CAKE (discovered|observation recovered)" $OUT/after-readd.log
check "pingers resume" [ "$(fpings)" = 1 ]
check "control resumes (new DATA records)" [ "$(grep -c '^DATA;' $OUT/after-readd.log)" -gt 5 ]
check "one injector after CAKE returns" [ "$(links)" = 1 ]
tc -d qdisc show dev ifb4eth1 > $OUT/ifb-cake-after

# 5. SQM restart replaces both qdiscs
before=$(wc -l < $LOG)
/etc/init.d/sqm restart; mark sqm-restart; sleep 12
tail -n +$before $LOG > $OUT/after-sqm.log
check "daemon survives SQM restart" [ -n "$(pid)" ]
check "pingers running after SQM restart" [ "$(fpings)" = 1 ]
check "data flows after SQM restart" [ "$(grep -c '^DATA;' $OUT/after-sqm.log)" -gt 5 ]
check "one injector after SQM restart" [ "$(links)" = 1 ]
tc qdisc show | grep cake > $OUT/qdiscs-after-sqm

# 6. Signals: export and reset keep the inode
inode=$(ls -i $LOG | awk '{print $1}')
exports_before=$(ls /var/log/cake-adapt_*.log* 2>/dev/null | wc -l)
kill -USR1 $(pid); sleep 3; mark usr1
ls -la /var/log/ > $OUT/var-log-after-usr1
check "SIGUSR1 exports logs" [ "$(ls /var/log/cake-adapt_*.log* 2>/dev/null | wc -l)" -gt "$exports_before" ]
kill -USR2 $(pid); sleep 3; mark usr2
check "SIGUSR2 keeps the inode" [ "$(ls -i $LOG | awk '{print $1}')" = "$inode" ]
check "SIGUSR2 resets the log" [ "$(wc -c < $LOG)" -lt 200000 ]

# 7. Rotation by size keeps the inode
uci set cake-adapt.main.log_file_max_size_KB='64'; uci commit cake-adapt
/etc/init.d/cake-adapt restart; sleep 2
inode=$(ls -i $LOG | awk '{print $1}')
wait_log "rotating log file" 120; mark rotation
check "size rotation happens" ls $LOG.old >/dev/null 2>&1
check "rotation keeps the inode" [ "$(ls -i $LOG | awk '{print $1}')" = "$inode" ]
grep -h "rotating log file" $LOG $LOG.old 2>/dev/null | head -2 > $OUT/rotation-messages
cp $OUT/cake-adapt.config-original /etc/config/cake-adapt
/etc/init.d/cake-adapt restart; sleep 4

# 8. Disabled and invalid configs
uci set cake-adapt.main.enabled='0'; uci commit cake-adapt
/etc/init.d/cake-adapt restart; sleep 3; mark disabled
check "disabled config does not run" [ -z "$(pid)" ]
check "disabled leaves no injector" [ "$(links)" = 0 ]
logread -e cake-adapt | tail -3 > $OUT/disabled-syslog
check "disabled is in syslog" grep -qi "disabled" $OUT/disabled-syslog
uci set cake-adapt.main.enabled='1'; uci set cake-adapt.main.min_dl_shaper_rate_kbps='90000'; uci commit cake-adapt
/etc/init.d/cake-adapt restart; sleep 3; mark invalid
logread -e cake-adapt | tail -4 > $OUT/invalid-syslog
check "invalid config is refused" [ -z "$(pid)" ]
check "invalid config leaves no injector" [ "$(links)" = 0 ]
check "invalid config is in syslog" grep -q "minimum <= base <= maximum" $OUT/invalid-syslog
cp $OUT/cake-adapt.config-original /etc/config/cake-adapt
/etc/init.d/cake-adapt restart; sleep 6; mark restored
check "restored config runs" [ -n "$(pid)" ]
check "restored config attaches one injector" [ "$(links)" = 1 ]
check "config file restored" cmp -s /etc/config/cake-adapt $OUT/cake-adapt.config-original
tc -d qdisc show > $OUT/qdiscs-final
logread -e cake-adapt > $OUT/syslog-all
echo "passed $pass failed $fail" | tee -a $OUT/summary
