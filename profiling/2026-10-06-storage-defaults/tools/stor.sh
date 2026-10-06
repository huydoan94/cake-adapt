#!/bin/sh
# Storage audit of the installed cake-adapt with the shipped default logging:
# the log goes to the default /var/log. The daemon runs on the emulated testbed
# (testbed.sh) through 12 minutes of load, export/reset, CAKE removal, idle
# sleep/wake and a time-based rotation, between 60 s and 90 s stopped windows
# (longer than the kernel's writeback delay). Block-device counters, dirty and
# writeback memory and storage IRQs are sampled every 10 s; the daemon and its
# children are traced from exec with every file, read, write, truncate, sync
# and map call, paths resolved. The installed service is stopped for the run
# and started again afterwards.
# This run logs to the default path on purpose, not to /tmp/sqm-mon-test.log,
# which it does not touch.
# usage: stor.sh RESULT_DIR defaults|tcp
set -u
R=$1 VARIANT=$2
DIR=/tmp/stor
X() { ip netns exec "$@"; }
mark() { printf '%s %s\n' "$(date +%s)" "$*" >> "$R/phases"; }
disk() {
	{
		echo "@ $(date +%s) $1"
		grep -E ' (sd[a-z]+[0-9]*|mmcblk[0-9]+(p[0-9]+)?|mtdblock[0-9]+|ubi[0-9_]+) ' /proc/diskstats
		grep -E '^(Dirty|Writeback):' /proc/meminfo
		grep -E 'ata_piix|ahci|mmc|nand|spi' /proc/interrupts
	} >> "$R/disk"
}
download() { X cpe iperf3 -c 10.99.0.2 -p 5202 -R -P 4 -t "$1" > /dev/null 2>&1; }
upload() { X cpe iperf3 -c 10.99.0.2 -p 5201 -t "$1" > /dev/null 2>&1; }

[ -z "$(ip netns list)" ] || { echo 'existing namespaces; refusing'; exit 1; }
[ ! -e "$R" ] || { echo "$R exists; refusing"; exit 1; }
mkdir -p "$R/uci"
cp "$DIR/uci-$VARIANT" "$R/uci/cake-adapt"
{
	uname -a; cat /proc/mounts; ls -ld /var /var/log /etc/TZ
	sysctl vm.dirty_expire_centisecs vm.dirty_writeback_centisecs
	ls -l /var/log
} > "$R/environment" 2>&1
/etc/init.d/cake-adapt stop; sleep 2
[ -z "$(pidof cake-adapt)" ] || { echo 'cake-adapt still running; refusing'; exit 1; }
sh "$DIR/testbed.sh" up
X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate 2500kbit burst 16k limit 156250 overhead 30 mpu 84 linklayer ethernet
X isp tc qdisc change dev iwan parent 1:1 handle 10: tbf rate 30000kbit burst 16k limit 1875000 overhead 30 mpu 84 linklayer ethernet
X cpe tc qdisc change dev cwan root cake bandwidth 2250kbit besteffort ack-filter ethernet overhead 44 mpu 84
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 27000kbit besteffort ethernet overhead 44 mpu 84
sync; sleep 5

( while :; do disk sample; sleep 10; done ) & SAMPLER=$!
mark control-before; disk control-before-start; sleep 60; disk control-before-end
mark start
strace -f -ttt -yy -s 96 -o "$R/trace" \
	-e trace=%file,read,readv,pread64,write,writev,pwrite64,pwritev,mmap2,truncate,ftruncate,fsync,fdatasync,sync,syncfs,sync_file_range \
	ip netns exec cpe /usr/sbin/cake-adapt -C "$R/uci" -S main > "$R/console" 2>&1 & TRACE=$!
sleep 10
DAEMON=$(pidof cake-adapt)
echo "$DAEMON" > "$R/daemon-pid"
mark idle;           sleep 50
mark download;       download 60
mark upload;         upload 60
mark bidirectional;  download 60 & D=$!; upload 60; wait "$D"
mark log-export;     kill -USR1 "$DAEMON"; sleep 5
mark log-reset;      kill -USR2 "$DAEMON"; sleep 5
mark cake-removed;   X cpe tc qdisc del dev cwan root; sleep 5
mark cake-recreated; X cpe tc qdisc add dev cwan root cake bandwidth 2250kbit besteffort ack-filter ethernet overhead 44 mpu 84; sleep 10
mark idle-sleep;     sleep 90
mark wake;           download 30
mark "idle-until-time-rotation"; sleep 240
mark stop
kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
wait "$TRACE"; mark "stopped trace-exit=$?"
disk control-after-start; sleep 90; disk control-after-end
mark end
kill "$SAMPLER"; wait "$SAMPLER" 2>/dev/null
ls -l /var/log > "$R/var-log-after"
sh "$DIR/testbed.sh" down
/etc/init.d/cake-adapt start; sleep 3
echo "service=$(pidof cake-adapt) namespaces=[$(ip netns list)]" > "$R/restored"
gzip -9 "$R/trace"
