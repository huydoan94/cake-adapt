#!/bin/sh
# Log directory from /etc/cake-adapt/exec (2026-10-21).
set -u
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fi; }
ipid() { cat /var/run/cake-adapt/main.lock 2>/dev/null; }
apk add --allow-untrusted /tmp/exec-move/cake-adapt-mqtt.apk >/dev/null 2>&1
/etc/init.d/cake-adapt restart; sleep 8
uci -c /etc/cake-adapt set exec.main=cake_adapt_exec
uci -c /etc/cake-adapt set exec.main.log_file_path_override=/tmp/ca-logtest
uci -c /etc/cake-adapt commit exec
mkdir -p /tmp/ca-logtest
/etc/init.d/cake-adapt reload; sleep 8
check "log in the root-only directory" '[ -s /tmp/ca-logtest/cake-adapt.log ]'
P=$(ipid)
uci set cake-adapt.main.log_file_path_override=/etc/hotplug.d/iface; uci commit cake-adapt; sleep 8
/etc/init.d/cake-adapt reload; sleep 6
check "kept running" '[ "$(ipid)" = "$P" ] && kill -0 "$P"'
check "refusal logged" 'logread | grep -q "option .log_file_path_override. is refused in /etc/config/cake-adapt"'
check "nothing written there" '! ls /etc/hotplug.d/iface | grep -q cake-adapt'
uci delete cake-adapt.main.log_file_path_override; uci commit cake-adapt
uci -c /etc/cake-adapt delete exec.main; uci -c /etc/cake-adapt commit exec
sleep 8; /etc/init.d/cake-adapt restart; sleep 6; rm -rf /tmp/ca-logtest
check "back to default log" 'kill -0 "$(ipid)" && [ -e /var/log/cake-adapt.log ]'
