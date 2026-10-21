#!/bin/sh
set -eu
task_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$task_dir/children.sh"
R=$(mktemp -d /tmp/native-proc-test.XXXXXX)
DAEMON=
cleanup() {
    if [ -n "$DAEMON" ]; then
        for pid in $(children "$DAEMON"); do kill "$pid" 2>/dev/null || true; done
        kill "$DAEMON" 2>/dev/null || true
        wait "$DAEMON" 2>/dev/null || true
    fi
    rm -rf "$R"
}
trap cleanup EXIT
now() { cut -d' ' -f1 /proc/uptime; }
cpu() {
    # Parent waited-child counters and live child identities must stay stable.
    for attempt in 1 2 3; do
        children_before=$(children "$DAEMON")
        cat "/proc/$DAEMON/stat" > "$R/cpu-parent-before"
        : > "$R/cpu-children"
        stable=1
        for pid in $children_before; do
            cat "/proc/$pid/stat" >> "$R/cpu-children" 2>/dev/null || stable=0
        done
        cat "/proc/$DAEMON/stat" > "$R/cpu-parent-after"
        children_after=$(children "$DAEMON")
        waited_before=$(awk '{print $16, $17}' "$R/cpu-parent-before")
        waited_after=$(awk '{print $16, $17}' "$R/cpu-parent-after")
        if [ "$stable" = 1 ] && [ "$children_before" = "$children_after" ] && [ "$waited_before" = "$waited_after" ]; then
            now > "$R/cpu-$1.time"
            cat "$R/cpu-parent-after" "$R/cpu-children" > "$R/cpu-$1.stat"
            rm "$R/cpu-parent-before" "$R/cpu-parent-after" "$R/cpu-children"
            return
        fi
    done
    echo 'CPU snapshot unstable after three attempts' >&2
    return 1
}
sh -c 'sleep 20 & wait' &
DAEMON=$!
# Wait for the known child, bounded by three attempts with a 0.1s local wait.
for attempt in 1 2 3; do
    test -n "$(children "$DAEMON")" && break
    sleep 0.1
done
cpu before
awk 'NR == 1 {if ($2 != "(sh)") exit 1} NR == 2 {if ($2 != "(sleep)") exit 1} END {if (NR != 2) exit 1}' "$R/cpu-before.stat"
cpu after
test -s "$R/cpu-after.time"
echo retained > "$R/log"
ln "$R/log" "$R/hardlink"
identity=$(sh "$task_dir/log-inode.sh" "$R/log")
test "$identity" = "$(sh "$task_dir/log-inode.sh" "$R/hardlink")"
test "$(cat "$R/log")" = retained
if sh "$task_dir/log-inode.sh" "$R/missing" 2>/dev/null; then exit 1; fi
test ! -e "$R/missing"
echo 'proc test: child discovery, CPU snapshots and read-only hardlink identity pass'
