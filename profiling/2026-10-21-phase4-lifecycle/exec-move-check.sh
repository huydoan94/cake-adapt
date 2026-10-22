#!/bin/sh
# Focused check of /etc/cake-adapt/exec on VM .2 (2026-10-21); bounded.
set -u
ipid() { cat /var/run/cake-adapt/main.lock 2>/dev/null; }
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fi; }
settle() { sleep 8; }
apk add --allow-untrusted /tmp/exec-move/cake-adapt-mqtt.apk /tmp/exec-move/luci-app-cake-adapt.apk >/dev/null 2>&1
apk info -v 2>/dev/null | grep -E '^cake-adapt'
ls -ld /etc/cake-adapt /etc/cake-adapt/exec
check "exec file 0600 root" '[ "$(ls -l /etc/cake-adapt/exec | cut -c1-10)" = "-rw-------" ] && [ "$(ls -ln /etc/cake-adapt/exec | awk "{print \$3}")" = 0 ]'
check "old exec file gone" '[ ! -e /etc/config/cake-adapt-exec ]'
check "comment-only file parses" 'uci -q -c /etc/cake-adapt show exec >/dev/null'
/etc/init.d/cake-adapt restart; settle
P0=$(ipid); echo "instance $P0"
check "running native" 'kill -0 "$P0" && ! pidof fping >/dev/null'
# fping with extra args from the root-only file, reloaded by hand.
printf "no_pingers=5\n" > /etc/cake-adapt/config.main.sh
uci set cake-adapt.main.pinger_method=fping; uci commit cake-adapt; settle
uci -c /etc/cake-adapt set exec.main=cake_adapt_exec
uci -c /etc/cake-adapt set exec.main.ping_extra_args='-S 10.0.2.15'
uci -c /etc/cake-adapt set exec.main.config_file=/etc/cake-adapt/config.main.sh
uci -c /etc/cake-adapt commit exec
/etc/init.d/cake-adapt reload; settle
P1=$(ipid); echo "instance $P1"
check "runtime has extra args" 'grep -q "ping_extra_args.*-S 10.0.2.15" /var/run/cake-adapt/main.uci'
check "runtime has file value" 'grep -q "no_pingers.*5" /var/run/cake-adapt/main.uci'
check "fping got the args" 'tr "\0" " " < /proc/$(pidof fping)/cmdline | grep -q -- "-S 10.0.2.15"'
check "exec file still 0600" '[ "$(ls -l /etc/cake-adapt/exec | cut -c1-10)" = "-rw-------" ]'
# ping_extra_args in the LuCI-writable file is refused; the instance keeps running.
uci set cake-adapt.main.ping_extra_args='-o /tmp/planted'; uci commit cake-adapt; settle
/etc/init.d/cake-adapt reload; settle
check "kept running" '[ "$(ipid)" = "$P1" ] && kill -0 "$P1"'
logread | grep -m1 "option 'ping_extra_args' is refused in /etc/config/cake-adapt" | cut -c1-200
check "refusal logged" 'logread | grep -q "option .ping_extra_args. is refused in /etc/config/cake-adapt"'
check "nothing planted" '[ ! -e /tmp/planted ]'
# The daemon run by hand on /etc/config/cake-adapt refuses it too.
/usr/sbin/cake-adapt -S main -V > /tmp/exec-move/manual 2>&1; echo "manual rc=$?"
check "manual -V refused" 'grep -q "ping_extra_args. is refused in /etc/config/cake-adapt" /tmp/exec-move/manual'
# Restore: native, empty exec file, no shell file.
uci delete cake-adapt.main.ping_extra_args; uci delete cake-adapt.main.pinger_method; uci commit cake-adapt
uci -c /etc/cake-adapt delete exec.main; uci -c /etc/cake-adapt commit exec
rm -f /etc/cake-adapt/config.main.sh
settle; /etc/init.d/cake-adapt restart; settle
check "back on native" 'kill -0 "$(ipid)" && ! pidof fping >/dev/null && ! grep -q config_file /var/run/cake-adapt/main.uci'
