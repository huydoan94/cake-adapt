#!/bin/sh
# sdk-build.sh: cleans and compiles the cake-adapt package in an OpenWrt SDK,
# then says why it failed, if it did.
#
# Usage: sh tools/sdk-build.sh NAME|DIRECTORY
#
# NAME is an SDK as sdk-resolve.sh finds it: x86 is $OPENWRT_SDK_X86 or
# ../openwrt-sdk-x86, mips is $OPENWRT_SDK_MIPS or ../openwrt-sdk-mips; an
# argument with a slash is the SDK directory. The SDK's package/cake-adapt
# must be a link to this repository. The full output is shown and also kept in
# build/sdk-<name>.log; the built package is copied to bin/ with -<name> before
# its extension, for example bin/cake-adapt-0.3.9-r1-filogic.apk.
#
# Exit status: 0 built, 2 SDK missing or not set up, 3 dependencies missing,
# 4 compile error, 1 any other failure.
set -u

repo=$(cd "$(dirname "$0")/.." && pwd)
[ $# -eq 1 ] && [ -n "$1" ] || {
	echo "usage: $0 NAME|DIRECTORY   (for example x86, filogic or ../openwrt-sdk-mips)" >&2
	exit 1
}
. "$repo/tools/sdk-resolve.sh"
sdk_resolve "$1"

fail() { # STATUS MESSAGE...
	status=$1
	shift
	printf '\nCANNOT BUILD (%s): ' "$name" >&2
	printf '%s\n' "$@" >&2
	exit "$status"
}

sdk_check
[ -f "$sdk/include/bpf.mk" ] ||
	fail 2 "the SDK at $sdk has no include/bpf.mk, which the package needs for its BPF object"
link=$(readlink -f "$sdk/package/cake-adapt" 2>/dev/null)
[ "$link" = "$repo" ] ||
	fail 2 "$sdk/package/cake-adapt does not point to this repository (it is: ${link:-missing})" \
		"Fix it with: ln -sfn $repo $sdk/package/cake-adapt"

mkdir -p "$repo/build"
log=$repo/build/sdk-$name.log
status_file=$repo/build/sdk-$name.status
echo "Building cake-adapt in $sdk (log: build/sdk-$name.log)"
{
	make -C "$sdk" package/cake-adapt/clean V=s &&
		make -C "$sdk" package/cake-adapt/compile V=s
	echo $? >"$status_file"
} 2>&1 | tee "$log"
status=$(cat "$status_file")
rm -f "$status_file"

# Dependencies the SDK's package index lacks. Runtime-only ones (fping, bash)
# do not stop the build: the package still records them, and the router
# installs them from its own feeds.
missing=$(sed -n "s/.*Makefile 'package\/cake-adapt\/Makefile' has a dependency on '\([^']*\)', which does not exist.*/\1/p" "$log" |
	sort -u | tr '\n' ' ')
libraries=$(grep -A3 'Package cake-adapt is missing dependencies' "$log" | grep -v 'Package cake-adapt' | tr -s ' \n' ' ')
headers=$(sed -n 's/.*fatal error: \([^:]*\): No such file or directory.*/\1/p' "$log" | sort -u | tr '\n' ' ')

if [ "$status" -eq 0 ] && [ -z "$libraries" ]; then
	echo
	# The newest package in the SDK's bin/, which is the one just built.
	package=$(find "$sdk/bin/packages" \( -name 'cake-adapt-*.apk' -o -name 'cake-adapt_*.ipk' \) \
		-printf '%T@ %p\n' | sort -rn | head -n 1 | cut -d' ' -f2-)
	[ -n "$package" ] || fail 1 "make succeeded but no cake-adapt package is in $sdk/bin/packages"
	# The SDK name goes before the extension: x86 and Filogic packages share a file name.
	file=$(basename "$package")
	copy=$repo/bin/${file%.*}-$name.${file##*.}
	mkdir -p "$repo/bin"
	cp "$package" "$copy"
	[ "$(sha256sum <"$package")" = "$(sha256sum <"$copy")" ] ||
		fail 1 "the copy $copy does not match $package"
	echo "BUILT ($name):"
	sha256sum "$package"
	echo "copied to bin/$(basename "$copy") (checksum verified)"
	[ -z "$missing" ] ||
		printf 'warning: the SDK has no package for runtime dependencies: %s\n%s\n' "$missing" \
			"  (harmless for this build; to clear it: cd $sdk && ./scripts/feeds install $missing)"
	exit 0
fi
[ -z "$libraries" ] ||
	fail 3 "the package is missing libraries: $libraries" \
		"Install their feed packages in the SDK and rebuild."
[ -z "$headers" ] ||
	fail 3 "headers not found: $headers" \
		"A dependency (libbpf, libnl-tiny, libuci, libubox, zlib) is probably not installed from the SDK's feeds."
errors=$(grep -E '(error|Error):' "$log" | grep -v -E '^make(\[[0-9]+\])?: \*\*\*|^ERROR: package/' | head -n 20)
[ -z "$errors" ] ||
	fail 4 "compile errors:" "$errors"
[ -z "$missing" ] ||
	fail 3 "the SDK lacks packages cake-adapt depends on: $missing" \
		"Install them with: (cd $sdk && ./scripts/feeds update -a && ./scripts/feeds install $missing)"
fail 1 "make exited with status $status; see build/sdk-$name.log"
