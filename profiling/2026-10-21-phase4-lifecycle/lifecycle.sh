#!/bin/sh
# Phase-4 lifecycle check on VM .2 (2026-10-21): upgrade to the build with
# native as the default pinger and the root-only exec file, then start,
# reload, exec-file change, refused option, fping fallback, ubusd restart,
# stop and start. Bounded waits only; results in /root/phase4-lifecycle.
set -u
R=/root/phase4-lifecycle
LOG=/tmp/sqm-mon-test.log
LOGDIR=/tmp/cake-adapt-lifecycle
mkdir -p "$R"
exec > "$R/steps" 2>&1
step() { echo "== $(cut -d' ' -f1 /proc/uptime) $*"; }
pid() { pidof cake-adapt || echo none; }
# Waits up to $1 s for the shell condition $2.
wait_for() {
	i=0
	while [ "$i" -lt "$1" ]; do
		eval "$2" && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}
data_count() { grep -c '^DATA;' "$LOG" 2>/dev/null || echo 0; }
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fi; }

step before
cat /proc/sys/kernel/random/boot_id
apk info -v 2>/dev/null | grep -E '^(cake-adapt|luci-app-cake|fping|bash)'
pid
uci show cake-adapt > "$R/uci-before"

step "test log, created once, inode kept"
[ -e "$LOG" ] || touch "$LOG"
ls -i "$LOG" | tee "$R/log-inode-before"
: > "$LOG"
mkdir -p "$LOGDIR"
ln -f "$LOG" "$LOGDIR/cake-adapt.log"
uci set cake-adapt.main.log_file_path_override="$LOGDIR"
uci commit cake-adapt

step upgrade
apk add --allow-untrusted /tmp/phase4/cake-adapt-mqtt.apk /tmp/phase4/luci-app-cake-adapt.apk
apk info -v 2>/dev/null | grep -E '^(cake-adapt|luci-app-cake|fping)'
ls -l /etc/config/cake-adapt /etc/config/cake-adapt-exec /usr/libexec/cake-adapt/
check "fping kept" 'command -v fping >/dev/null'
check "exec file 0600" '[ "$(ls -l /etc/config/cake-adapt-exec | cut -c1-10)" = "-rw-------" ]'
check "config kept" 'uci -q get cake-adapt.main.tcp_ts_request >/dev/null'

step "restart on the new build: native by default"
/etc/init.d/cake-adapt restart
wait_for 20 '[ "$(data_count)" -gt 20 ]'
P1=$(pid)
echo "pid $P1, DATA $(data_count)"
check "running" '[ "$P1" != none ]'
check "no fping child" '! pidof fping >/dev/null'
check "native samples" '[ "$(data_count)" -gt 20 ]'
grep -m3 -i 'pinger\|native' "$LOG"
grep -c 'pinger_method' /var/run/cake-adapt/main.uci || :
ubus call cake-adapt.main status > "$R/status-native" 2>&1
check "ubus status" 'grep -q "\"pinger\|\"latency\|\"state\"" "$R/status-native"'
curl -s -m 5 http://127.0.0.1/ubus -d '{"jsonrpc":"2.0","id":1,"method":"call","params":["00000000000000000000000000000000","luci","getFeatures",{}]}' > "$R/luci-features" 2>&1
check "LuCI answers" 'grep -q "jsonrpc" "$R/luci-features"'

step "reload without a change keeps the instance"
/etc/init.d/cake-adapt reload
sleep 3
check "same pid after reload" '[ "$(pid)" = "$P1" ]'

step "exec file change: config_file fills an unset option, reload restarts"
printf '%s\n' 'no_pingers=5' > /root/phase4-config.sh
uci set cake-adapt-exec.main=cake_adapt_exec
uci set cake-adapt-exec.main.config_file=/root/phase4-config.sh
uci commit cake-adapt-exec
/etc/init.d/cake-adapt reload
wait_for 15 '[ "$(pid)" != "$P1" ] && [ "$(pid)" != none ]'
P2=$(pid)
check "restarted by exec change" '[ "$P2" != "$P1" ] && [ "$P2" != none ]'
check "file value imported" 'grep -q "no_pingers.*5" /var/run/cake-adapt/main.uci'
check "config_file in runtime" 'grep -q "config_file.*phase4-config" /var/run/cake-adapt/main.uci'

step "config_file in /etc/config/cake-adapt is refused; instance keeps running"
uci set cake-adapt.main.config_file=/root/phase4-config.sh
uci commit cake-adapt
/etc/init.d/cake-adapt reload
sleep 5
check "kept running" '[ "$(pid)" = "$P2" ]'
logread | grep -m2 "read only from /etc/config/cake-adapt-exec" | tee "$R/refusal"
check "refusal logged" '[ -s "$R/refusal" ]'
uci delete cake-adapt.main.config_file
uci commit cake-adapt

step "fping fallback"
uci set cake-adapt.main.pinger_method=fping
uci commit cake-adapt
/etc/init.d/cake-adapt reload
wait_for 15 'pidof fping >/dev/null'
check "fping child running" 'pidof fping >/dev/null'
C0=$(data_count)
sleep 5
check "fping samples" '[ "$(data_count)" -gt "$C0" ]'
uci delete cake-adapt.main.pinger_method
uci commit cake-adapt
/etc/init.d/cake-adapt reload
wait_for 15 '! pidof fping >/dev/null'
check "back to native" '! pidof fping >/dev/null && [ "$(pid)" != none ]'

step "ubusd restart: the daemon keeps running"
P3=$(pid)
C0=$(data_count)
kill "$(pidof ubusd)"
wait_for 15 'pidof ubusd >/dev/null'
sleep 5
check "same daemon" '[ "$(pid)" = "$P3" ]'
check "still sampling" '[ "$(data_count)" -gt "$C0" ]'
wait_for 20 'ubus call cake-adapt.main status >/dev/null 2>&1'
check "ubus object back" 'ubus call cake-adapt.main status >/dev/null 2>&1'
for service in rpcd dnsmasq odhcpd uhttpd; do /etc/init.d/$service restart; done
sleep 3
curl -s -m 5 http://127.0.0.1/ubus -d '{"jsonrpc":"2.0","id":1,"method":"call","params":["00000000000000000000000000000000","luci","getFeatures",{}]}' > "$R/luci-features-after" 2>&1
check "LuCI answers after" 'grep -q "jsonrpc" "$R/luci-features-after"'

step "stop and start"
/etc/init.d/cake-adapt stop
wait_for 10 '[ "$(pid)" = none ]'
check "stopped" '[ "$(pid)" = none ]'
/etc/init.d/cake-adapt start
wait_for 15 '[ "$(pid)" != none ]'
check "started" '[ "$(pid)" != none ]'

step "leave the VM on the new build: exec file emptied, log override removed"
uci delete cake-adapt-exec.main
uci commit cake-adapt-exec
uci delete cake-adapt.main.log_file_path_override
uci commit cake-adapt
/etc/init.d/cake-adapt restart
wait_for 15 '[ "$(pid)" != none ]'
rm -f /root/phase4-config.sh "$LOGDIR/cake-adapt.log"
rmdir "$LOGDIR" 2>/dev/null
ls -i "$LOG" | tee "$R/log-inode-after"
check "test-log inode kept" 'cmp -s "$R/log-inode-before" "$R/log-inode-after"'
pid
logread | grep cake-adapt | tail -n 40 > "$R/syslog-tail"
step done
