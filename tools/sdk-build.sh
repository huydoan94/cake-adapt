#!/bin/sh
# sdk-build.sh: cleans and compiles the cake-adapt package in an OpenWrt SDK,
# then says why it failed, if it did.
# Preflights and stages the daemon dependencies; never recurses into other packages.
#
# Usage: sh tools/sdk-build.sh [--prepare-only] [ssl=yes|no] NAME|DIRECTORY
#
# NAME is an SDK as sdk-resolve.sh finds it: x86 is $OPENWRT_SDK_X86 or
# ../openwrt-sdk-x86, mips is $OPENWRT_SDK_MIPS or ../openwrt-sdk-mips; an
# argument with a slash is the SDK directory. The SDK's package/cake-adapt
# must be a link to this repository. The full output is shown and also kept in
# build/sdk-<name>.log; the built package is copied to bin/ with -<name> before
# its extension, for example bin/cake-adapt-0.3.9-r1-filogic.apk. When the SDK's
# package/luci-app-cake-adapt links to this repository's luci-app-cake-adapt, that
# package is built too and copied to bin/ without an architecture suffix.
# --prepare-only stages dependencies without building or copying the apps.
# ssl=yes selects the SSL-capable Mosquitto library; ssl=no is the default.
# SSL build logs and copied daemon packages include an additional -ssl suffix.
# SSL builds allow opt-in MQTT TLS; runtime TLS remains off by default.
# Required tools must already be available. Only existing local feeds are
# registered; compilation may download sources missing from the SDK's dl/ cache.
#
# Exit status: 0 built, 2 SDK missing or not set up, 3 libraries or headers
# missing, 4 compile error, 1 any other failure.
set -u

repo=$(cd "$(dirname "$0")/.." && pwd)
prepare_only=
ssl=no
ssl_seen=
sdk_argument=
usage() {
	echo "usage: $0 [--prepare-only] [ssl=yes|no] NAME|DIRECTORY   (for example x86 or filogic)" >&2
	exit 1
}
for argument in "$@"; do
	case $argument in
	--prepare-only) [ -z "$prepare_only" ] || usage; prepare_only=1 ;;
	ssl=yes|ssl=no)
		[ -z "$ssl_seen" ] || usage
		ssl=${argument#ssl=}
		ssl_seen=1 ;;
	ssl=*|-*|*=*) usage ;;
	*) [ -z "$sdk_argument" ] && [ -n "$argument" ] || usage; sdk_argument=$argument ;;
	esac
done
[ -n "$sdk_argument" ] || usage
mqtt_variant=nossl
[ "$ssl" != yes ] || mqtt_variant=ssl
. "$repo/tools/sdk-resolve.sh"
sdk_resolve "$sdk_argument"

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

mqtt_staged() {
	[ -f "$staging/stamp/.mosquitto.${mqtt_variant}_installed" ] &&
		[ -f "$staging/usr/include/mosquitto.h" ] &&
		[ -f "$staging/usr/lib/libmosquitto.so" ] &&
		[ -f "$staging/usr/lib/libmosquitto.so.1" ] || return 1
	# SSL and non-SSL variants share a soname and can both have valid stamps.
	needed=$(readelf -d "$staging/usr/lib/libmosquitto.so.1") || return 1
	if [ "$ssl" = yes ]; then
		printf '%s\n' "$needed" | grep -Eq 'Shared library: \[libssl[.]' &&
			printf '%s\n' "$needed" | grep -Eq 'Shared library: \[libcrypto[.]'
	else
		! printf '%s\n' "$needed" | grep -Eq 'Shared library: \[lib(ssl|crypto)[.]'
	fi
}

. "$repo/tools/sdk-dependencies.sh"

mkdir -p "$repo/build"
build_name=$name
[ "$ssl" != yes ] || build_name=$name-ssl
log=$repo/build/sdk-$build_name.log
status_file=$repo/build/sdk-$build_name.status
contracts=$repo/build/sdk-$build_name.contracts
plan=$repo/build/sdk-$build_name.plan
if [ -n "$prepare_only" ]; then
	echo "Preparing dependencies in $sdk (ssl=$ssl; log: build/sdk-$build_name.log)"
else
	echo "Building cake-adapt in $sdk (ssl=$ssl; log: build/sdk-$build_name.log)"
fi
{
	(prepare_dependencies)
	echo $? >"$status_file"
} 2>&1 | tee "$log"
status=$(cat "$status_file")
rm -f "$status_file"
[ "$status" -eq 0 ] || fail "$status" "dependency preparation failed (see $log)"
if [ -n "$prepare_only" ]; then
	echo "READY ($name, ssl=$ssl): all daemon headers and libraries staged"
	exit 0
fi
{
	make -C "$sdk" package/cake-adapt/clean NO_DEPS=1 "ssl=$ssl" &&
		make -C "$sdk" package/cake-adapt/compile NO_DEPS=1 "ssl=$ssl"
	echo $? >"$status_file"
} 2>&1 | tee -a "$log"
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
	copy=$repo/bin/${file%.*}-$build_name.${file##*.}
	mkdir -p "$repo/bin"
	cp "$package" "$copy"
	[ "$(sha256sum <"$package")" = "$(sha256sum <"$copy")" ] ||
		fail 1 "the copy $copy does not match $package"
	echo "BUILT ($name, ssl=$ssl):"
	sha256sum "$package"
	echo "copied to bin/$(basename "$copy") (checksum verified)"
	# The LuCI app, when the SDK links it; its package has no architecture.
	luci_link=$(readlink -f "$sdk/package/luci-app-cake-adapt" 2>/dev/null)
	if [ "$luci_link" = "$repo/luci-app-cake-adapt" ]; then
		echo "Building luci-app-cake-adapt in $sdk"
		{
			make -C "$sdk" package/luci-app-cake-adapt/clean NO_DEPS=1 "ssl=$ssl" &&
				make -C "$sdk" package/luci-app-cake-adapt/compile NO_DEPS=1 "ssl=$ssl"
			echo $? >"$status_file"
		} 2>&1 | tee -a "$log"
		[ "$(cat "$status_file")" -eq 0 ] || fail 1 "the luci-app-cake-adapt build failed (see $log)"
		rm -f "$status_file"
		package=$(find "$sdk/bin/packages" \( -name 'luci-app-cake-adapt-*.apk' -o -name 'luci-app-cake-adapt_*.ipk' \) \
			-printf '%T@ %p\n' | sort -rn | head -n 1 | cut -d' ' -f2-)
		[ -n "$package" ] || fail 1 "make succeeded but no luci-app-cake-adapt package is in $sdk/bin/packages"
		file=$(basename "$package")
		copy=$repo/bin/$file
		[ "$ssl" != yes ] || copy=$repo/bin/${file%.*}-ssl.${file##*.}
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
		"A staged dependency disappeared or is incompatible; inspect the preflight plan and SDK feeds."
errors=$(grep -E '(error|Error):' "$log" | grep -v -E '^make(\[[0-9]+\])?: \*\*\*|^ERROR: package/' | head -n 20)
[ -z "$errors" ] ||
	fail 4 "compile errors:" "$errors"
# Missing runtime packages (fping, bash) only warn, so they do not explain a
# failed build; without V=s the compiler's own errors are not in the log.
fail 1 "make exited with status $status, and the log shows no compiler error" \
	"See the compiler output with: make -C $sdk package/cake-adapt/compile NO_DEPS=1 ssl=$ssl V=s"
