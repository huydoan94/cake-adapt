#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	printf 'usage: %s RUNNER OBJECT\n' "$0" >&2
	exit 2
fi

runner=$1
object=$2
namespace="cake-lifetime-$$"
created=0
cleanup()
{
	if [ "$created" -eq 1 ]; then
		ip netns del "$namespace" 2>/dev/null || true
	fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

command -v ip >/dev/null 2>&1 || { printf 'iproute2 is required\n' >&2; exit 1; }
[ -x "$runner" ] || { printf 'runner is not executable: %s\n' "$runner" >&2; exit 1; }
[ -r "$object" ] || { printf 'BPF object is not readable: %s\n' "$object" >&2; exit 1; }

ip netns add "$namespace"
created=1
ip -n "$namespace" link add ca0 type veth peer name ca1
ip -n "$namespace" link set ca0 address 02:00:00:00:00:01
ip -n "$namespace" link set ca1 address 02:00:00:00:00:02
ip -n "$namespace" link set ca0 up
ip -n "$namespace" link set ca1 up

# No addresses/routes are configured. The runner attaches the exact socket
# filter to ca0 and injects raw Ethernet/IPv4/TCP timestamp packets on both ends.
ip netns exec "$namespace" "$runner" "$object" ca0 ca1
