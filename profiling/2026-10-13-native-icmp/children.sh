#!/bin/sh
# OpenWrt may omit /proc/PID/task/PID/children. Read PPID from procfs instead.
children() {
    for stat_path in /proc/[0-9]*/stat; do
        { read -r stat_text < "$stat_path"; } 2>/dev/null || continue
        stat_tail=${stat_text##*) }
        parent=${stat_tail#* }
        parent=${parent%% *}
        if [ "$parent" = "$1" ]; then
            child=${stat_path#/proc/}
            echo "${child%/stat}"
        fi
    done
}
