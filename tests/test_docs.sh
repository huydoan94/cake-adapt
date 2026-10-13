#!/bin/sh
# The files every agent session reads stay small. Codex loads at most 32 KiB of
# AGENTS.md (project_doc_max_bytes) and drops the rest silently; the limits here
# leave room below that. Move detail to docs/agents/ or docs/history/ instead.
set -eu
cd "$(dirname "$0")/.."
status=0
limit() { # FILE BYTES
	size=$(wc -c <"$1")
	if [ "$size" -gt "$2" ]; then
		printf '%s is %s bytes, over its %s-byte limit\n' "$1" "$size" "$2" >&2
		status=1
	fi
}
limit AGENTS.md 24576
limit HANDOFF.md 8192
[ "$status" -eq 0 ] && printf '%s\n' 'agent docs within size limits'
exit "$status"
