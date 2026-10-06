#!/bin/sh
# Package install and procd lifecycle of the installed service on eth1.
set -u
S=/etc/init.d/cake-adapt
step() { echo "== $(date +%T) $*"; }
daemon() { pidof cake-adapt; }
fpings() { for p in $(pidof fping); do [ "$(awk '{print $4}' /proc/$p/stat)" = "$(daemon)" ] && printf '%s ' "$p"; done; }
wait_daemon() { for i in $(seq 1 15); do [ -n "$(daemon)" ] && [ -n "$(fpings)" ] && return 0; sleep 1; done; return 1; }
cp /etc/config/cake-adapt /tmp/cake-adapt.config.orig
step install; apk add --allow-untrusted --force-reinstall /tmp/cake-adapt-head.apk 2>&1 | tail -2
sha256sum /usr/sbin/cake-adapt /lib/bpf/cake-adapt-tcpdelay.o /etc/config/cake-adapt
cmp -s /etc/config/cake-adapt /tmp/cake-adapt.config.orig && echo config-preserved
$S stop; sleep 2
step start; $S start; wait_daemon && echo "running pid=$(daemon) fping=$(fpings)"
step restart; old=$(daemon); $S restart; sleep 1; wait_daemon; echo "old=$old new=$(daemon) fping=$(fpings) old-alive=$(kill -0 $old 2>/dev/null && echo yes || echo no)"
step respawn-after-kill9; old=$(daemon); kill -9 $old; sleep 6; wait_daemon; echo "old=$old new=$(daemon) fping=$(fpings) orphans=$(for p in $(pidof fping); do awk '{print $4}' /proc/$p/stat; done | tr '\n' ' ')"
step reload-on-config-change; old=$(daemon); uci set cake-adapt.main.no_pingers=4; uci commit cake-adapt; $S reload; sleep 1; wait_daemon; echo "old=$old new=$(daemon) runtime-no_pingers=$(grep no_pingers /tmp/cake-adapt-config/main/cake-adapt)"
step invalid-config; uci set cake-adapt.main.min_ul_shaper_rate_kbps=30000; uci commit cake-adapt; $S restart; sleep 3; echo "daemon=[$(daemon)] fping=[$(pidof fping)]"
step disabled; cp /tmp/cake-adapt.config.orig /etc/config/cake-adapt; uci set cake-adapt.main.enabled=0; uci commit cake-adapt; $S restart; sleep 3; echo "daemon=[$(daemon)]"
step restore-and-start; cp /tmp/cake-adapt.config.orig /etc/config/cake-adapt; $S restart; wait_daemon; echo "running pid=$(daemon) fping=$(fpings)"
step stop; $S stop; sleep 2; echo "daemon=[$(daemon)] fping=[$(pidof fping)]"
step final-start; $S start; wait_daemon; echo "running pid=$(daemon)"; cmp -s /etc/config/cake-adapt /tmp/cake-adapt.config.orig && echo config-restored
step syslog; logread | grep -E "cake-adapt" | grep -vE "^.*(DATA|LOAD|SUMMARY|REFLECTOR|SHAPER);" | tail -40
