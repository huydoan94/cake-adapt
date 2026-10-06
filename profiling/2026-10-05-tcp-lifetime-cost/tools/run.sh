#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
	printf 'usage: %s RUNNER OBJECT LABEL\n' "$0" >&2
	exit 2
fi

runner=$1
object=$2
label=$3
namespace="cake-cost-$$"
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

[ -x "$runner" ] || { printf 'runner unavailable\n' >&2; exit 1; }
[ -r "$object" ] || { printf 'object unavailable\n' >&2; exit 1; }
ip netns add "$namespace"
created=1
ip netns exec "$namespace" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
ip netns exec "$namespace" sysctl -qw net.ipv6.conf.default.disable_ipv6=1
ip -n "$namespace" link add ca0 type veth peer name ca1
ip -n "$namespace" link set ca0 address 02:00:00:00:00:01
ip -n "$namespace" link set ca1 address 02:00:00:00:00:02
ip -n "$namespace" link set ca0 up
ip -n "$namespace" link set ca1 up
printf 'RUN\t%s\n' "$label"
ip netns exec "$namespace" "$runner" "$object" ca0 ca1
