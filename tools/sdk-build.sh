#!/bin/sh
# sdk-build.sh: cleans and compiles the cake-adapt package in an OpenWrt SDK,
# then says why it failed, if it did.
# Dependencies must already be staged; builds do not recurse into other packages.
#
# Usage: sh tools/sdk-build.sh NAME|DIRECTORY
#
# NAME is an SDK as sdk-resolve.sh finds it: x86 is $OPENWRT_SDK_X86 or
# ../openwrt-sdk-x86, mips is $OPENWRT_SDK_MIPS or ../openwrt-sdk-mips; an
# argument with a slash is the SDK directory. The SDK's package/cake-adapt
# must be a link to this repository. The full output is shown and also kept in
# build/sdk-<name>.log; the built package is copied to bin/ with -<name> before
# its extension, for example bin/cake-adapt-0.3.9-r1-filogic.apk. When the SDK's
# package/luci-app-cake-adapt links to this repository's luci-app-cake-adapt, that
# package is built too and copied to bin/ as it is named (it has no architecture).
#
# Exit status: 0 built, 2 SDK missing or not set up, 3 libraries or headers
# missing, 4 compile error, 1 any other failure.
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

# Both Mosquitto variants install the same soname. Verify the actual staged
# library so an earlier SSL build cannot silently satisfy the non-SSL package.
library=$(ls "$sdk"/staging_dir/target-*/usr/lib/libmosquitto.so.1 2>/dev/null | head -n 1)
[ -n "$library" ] ||
	fail 3 "libmosquitto-nossl is not staged in this SDK; prepare its dependencies first"
command -v readelf >/dev/null || fail 2 "readelf is needed to verify the staged MQTT library"
needed=$(readelf -d "$library") || fail 3 "cannot inspect the staged MQTT library: $library"
if printf '%s\n' "$needed" | grep -Eq 'Shared library: \[lib(ssl|crypto)[.]'; then
	fail 3 "the staged Mosquitto library uses OpenSSL; stage libmosquitto-nossl first"
fi

mkdir -p "$repo/build"
log=$repo/build/sdk-$name.log
status_file=$repo/build/sdk-$name.status
echo "Building cake-adapt in $sdk (log: build/sdk-$name.log)"
{
	make -C "$sdk" package/cake-adapt/clean NO_DEPS=1 &&
		make -C "$sdk" package/cake-adapt/compile NO_DEPS=1
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
	# The LuCI app, when the SDK links it; its package has no architecture.
	luci_link=$(readlink -f "$sdk/package/luci-app-cake-adapt" 2>/dev/null)
	if [ "$luci_link" = "$repo/luci-app-cake-adapt" ]; then
		echo "Building luci-app-cake-adapt in $sdk"
		{
			make -C "$sdk" package/luci-app-cake-adapt/clean NO_DEPS=1 &&
				make -C "$sdk" package/luci-app-cake-adapt/compile NO_DEPS=1
			echo $? >"$status_file"
		} 2>&1 | tee -a "$log"
		[ "$(cat "$status_file")" -eq 0 ] || fail 1 "the luci-app-cake-adapt build failed (see $log)"
		rm -f "$status_file"
		package=$(find "$sdk/bin/packages" \( -name 'luci-app-cake-adapt-*.apk' -o -name 'luci-app-cake-adapt_*.ipk' \) \
			-printf '%T@ %p\n' | sort -rn | head -n 1 | cut -d' ' -f2-)
		[ -n "$package" ] || fail 1 "make succeeded but no luci-app-cake-adapt package is in $sdk/bin/packages"
		copy=$repo/bin/$(basename "$package")
		cp "$package" "$copy"
		[ "$(sha256sum <"$package")" = "$(sha256sum <"$copy")" ] ||
			fail 1 "the copy $copy does not match $package"
		sha256sum "$package"
		echo "copied to bin/$(basename "$copy") (checksum verified)"
	fi
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
		"A dependency (libbpf, libnl-tiny, libuci, libubox, zlib, libmosquitto-nossl) is probably not staged from the SDK's feeds."
errors=$(grep -E '(error|Error):' "$log" | grep -v -E '^make(\[[0-9]+\])?: \*\*\*|^ERROR: package/' | head -n 20)
[ -z "$errors" ] ||
	fail 4 "compile errors:" "$errors"
# Missing runtime packages (fping, bash) only warn, so they do not explain a
# failed build; without V=s the compiler's own errors are not in the log.
fail 1 "make exited with status $status, and the log shows no compiler error" \
	"See the compiler output with: make -C $sdk package/cake-adapt/compile NO_DEPS=1 V=s"
