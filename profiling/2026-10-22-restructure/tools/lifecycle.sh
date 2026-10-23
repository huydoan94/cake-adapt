#!/bin/sh
# Lifecycle check of the restructured 0.3.26 tree on VM .2 (2026-10-22), from
# the 0.3.26 run (profiling/2026-10-22-e2e-0.3.26), itself from the 2026-10-21 phase-4
# script: upgrade, start, -D defaults (direct and through rpcd's file exec),
# reload, exec-file change, refused option, fping fallback, ubusd restart,
# stop and start. Root-only options live in /etc/cake-adapt/exec. Bounded
# waits only; results in /root/e2e-restr. The packages have the same version
# as the installed ones, so they are reinstalled and the binary checked.
set -u
R=/root/e2e-restr
LOG=/tmp/sqm-mon-test.log
LOGDIR=/tmp/cake-adapt-lifecycle
mkdir -p "$R"
exec > "$R/steps" 2>&1
step() { echo "== $(cut -d' ' -f1 /proc/uptime) $*"; }
# The running daemon's pid, from its instance lock: pidof also finds the
# init script's validation run (cake-adapt -V) and a daemon still exiting.
pid() {
	p=$(cat /var/run/cake-adapt/main.lock 2>/dev/null)
	if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then echo "$p"; else echo none; fi
}
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
data_count() { n=$(grep -c '^DATA;' "$LOG" 2>/dev/null); echo "${n:-0}"; }
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
# Root-only, so set in the exec file.
exec_uci() { uci -c /etc/cake-adapt "$@"; }
exec_uci set exec.main=cake_adapt_exec
exec_uci set exec.main.log_file_path_override="$LOGDIR"
exec_uci commit exec

ubus call cake-adapt.main status > "$R/status-before" 2>&1
sha256sum /usr/sbin/cake-adapt | cut -c1-16 > "$R/binary-before"

step upgrade
apk add --allow-untrusted --force-reinstall /tmp/e2e/cake-adapt-mqtt.apk /tmp/e2e/luci-app-cake-adapt.apk
apk info -v 2>/dev/null | grep -E '^(cake-adapt|luci-app-cake|fping)'
ls -l /etc/config/cake-adapt /etc/cake-adapt/exec /usr/libexec/cake-adapt/
check "version 0.3.26" 'apk info -v 2>/dev/null | grep -q "^cake-adapt-mqtt-0.3.26"'
check "binary replaced" '[ "$(sha256sum /usr/sbin/cake-adapt | cut -c1-16)" != "$(cat "$R/binary-before")" ]'
check "fping kept" 'command -v fping >/dev/null'
check "exec file 0600" '[ "$(ls -l /etc/cake-adapt/exec | cut -c1-10)" = "-rw-------" ]'

step "defaults: -D, and through rpcd as LuCI asks for them"
/usr/sbin/cake-adapt -D > "$R/defaults"
check "-D lists every option" '[ "$(wc -l < "$R/defaults")" -ge 80 ]'
check "-D native default" 'grep -qx "pinger_method=native" "$R/defaults"'
check "-D alone" '! /usr/sbin/cake-adapt -D -L >/dev/null 2>&1'
ubus call file exec '{"command":"/usr/sbin/cake-adapt","params":["-D"]}' > "$R/defaults-rpcd" 2>&1
check "rpcd runs -D" 'grep -q "pinger_method=native" "$R/defaults-rpcd"'
grep -q '"/usr/sbin/cake-adapt -D"' /usr/share/rpcd/acl.d/luci-app-cake-adapt.json
check "ACL grants -D" '[ $? -eq 0 ]'

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
printf '%s\n' 'no_pingers=5' > /root/e2e-config.sh
exec_uci set exec.main.config_file=/root/e2e-config.sh
exec_uci commit exec
/etc/init.d/cake-adapt reload
wait_for 15 '[ "$(pid)" != "$P1" ] && [ "$(pid)" != none ]'
P2=$(pid)
check "restarted by exec change" '[ "$P2" != "$P1" ] && [ "$P2" != none ]'
check "file value imported" 'grep -q "no_pingers.*5" /var/run/cake-adapt/main.uci'
check "config_file in runtime" 'grep -q "config_file.*e2e-config" /var/run/cake-adapt/main.uci'

step "config_file in /etc/config/cake-adapt is refused; instance keeps running"
uci set cake-adapt.main.config_file=/root/e2e-config.sh
uci commit cake-adapt
/etc/init.d/cake-adapt reload
sleep 5
check "kept running" '[ "$(pid)" = "$P2" ]'
logread | grep -m2 "is refused in /etc/config/cake-adapt" | tee "$R/refusal"
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
exec_uci delete exec.main
exec_uci commit exec
/etc/init.d/cake-adapt restart
wait_for 15 '[ "$(pid)" != none ]'
rm -f /root/e2e-config.sh "$LOGDIR/cake-adapt.log"
rmdir "$LOGDIR" 2>/dev/null
ls -i "$LOG" | tee "$R/log-inode-after"
check "test-log inode kept" 'cmp -s "$R/log-inode-before" "$R/log-inode-after"'
pid
logread | grep cake-adapt | tail -n 40 > "$R/syslog-tail"
step done
