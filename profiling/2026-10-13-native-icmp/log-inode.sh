#!/bin/sh
set -eu
# Read-only descriptor; filesystem device+inode survives mount namespace aliases.
exec 9< "${1:-/tmp/sqm-mon-test.log}"
mount_id=$(awk '/^mnt_id:/ {print $2}' /proc/$$/fdinfo/9)
inode=$(awk '/^ino:/ {print $2}' /proc/$$/fdinfo/9)
device=$(awk -v id="$mount_id" '$1 == id {print $3; exit}' /proc/$$/mountinfo)
test -n "$device" && test -n "$inode"
echo "$device:$inode"
