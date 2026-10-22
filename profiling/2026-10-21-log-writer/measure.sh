#!/bin/sh
# Does a stuck logd hold up cake-adapt's main loop? (2026-10-21)
# Usage: measure.sh LABEL. Debug messages go to syslog, logd is paused for
# 20 s, and 10 export requests 1 s apart then make more syslog calls than the kernel
# queues for it. The status's delay_us changes with
# nearly every sample, so the number of distinct values per window shows
# whether the main loop kept working. Bounded; logd is always resumed.
set -u
label=$1
out=/root/log-writer-$label
mkdir -p "$out"
trap 'kill -CONT $(pidof logd) 2>/dev/null' EXIT INT TERM
uci set cake-adapt.main.log_DEBUG_messages_to_syslog=1
uci commit cake-adapt
sleep 8
/etc/init.d/cake-adapt restart
sleep 10
apk info -v 2>/dev/null | grep '^cake-adapt' > "$out/version"
sample() {
	for i in $(seq 1 "$2"); do
		echo "$1 $(cut -d' ' -f1 /proc/uptime) $(ubus -t 2 call cake-adapt.main status 2>/dev/null |
			jsonfilter -e '@.download.delay_us' -e '@.state' | tr '\n' ' ')"
		sleep 1
	done
}
{
	sample before 10
	kill -STOP "$(pidof logd)"
	# Each export request logs debug lines to syslog: with logd paused, the
	# kernel queues only max_dgram_qlen (10) of them, so these force the limit.
	pid=$(cat /var/run/cake-adapt/main.lock)
	( for i in $(seq 1 10); do kill -USR1 "$pid"; sleep 1; done ) &
	sample paused 20
	kill -CONT "$(pidof logd)"
	sample after 10
} > "$out/samples"
for window in before paused after; do
	echo "$window: $(awk -v w=$window '$1==w {print $3}' "$out/samples" | sort -u | grep -c .) distinct of $(grep -c "^$window " "$out/samples") samples"
done | tee "$out/summary"
logread | grep cake-adapt | grep -E 'dropped|writer' | tail -n 5 >> "$out/summary"
pid=$(cat /var/run/cake-adapt/main.lock)
echo "daemon $pid alive=$(kill -0 "$pid" && echo yes)" >> "$out/summary"
rm -f /var/log/cake-adapt_*
uci delete cake-adapt.main.log_DEBUG_messages_to_syslog
uci commit cake-adapt
sleep 8
/etc/init.d/cake-adapt restart
