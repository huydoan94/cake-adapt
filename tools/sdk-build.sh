#!/bin/sh
# sdk-build.sh: cleans and compiles the cake-adapt package variants in an
# OpenWrt SDK, then says why a build failed, if one did.
# Preflights and stages each variant's dependencies; never recurses into other packages.
#
# Usage: sh tools/sdk-build.sh [--prepare-only] [variant=base|mqtt|mqtt-ssl ...] NAME|DIRECTORY
#
# NAME is an SDK as sdk-resolve.sh finds it: x86 is $OPENWRT_SDK_X86 or
# ../openwrt-sdk-x86, mips is $OPENWRT_SDK_MIPS or ../openwrt-sdk-mips; an
# argument with a slash is the SDK directory. The SDK's package/cake-adapt
# must be a link to this repository.
#
# The root Makefile builds three variants of the daemon: base is the package
# cake-adapt (no MQTT, no libmosquitto), mqtt is cake-adapt-mqtt
# (libmosquitto-nossl) and mqtt-ssl is cake-adapt-mqtt-ssl (libmosquitto-ssl,
# OpenSSL and ca-bundle). Each variant= argument selects one; without any, all
# three are built, in that order. Each variant's dependencies are prepared just
# before it is built: the two MQTT variants share the staged libmosquitto, so the
# last MQTT variant's library is the one left staged.
#
# The output of make goes only to build/sdk-<name>-<variant>.log; the terminal
# shows this script's own messages and, when a build fails, the errors found in
# the log and its last 30 lines. VERBOSE=1 also shows the full make output.
# Each built package is copied to bin/ with -<name> before its extension, for
# example bin/cake-adapt-mqtt-0.3.17-r1-x86.apk. When the SDK's
# package/luci-app-cake-adapt links to this repository's luci-app-cake-adapt,
# that package is built once after the daemon (log build/sdk-<name>-luci.log)
# and copied to bin/ without an architecture suffix; it adapts at run time to
# whichever variant is installed.
# --prepare-only stages each selected variant's dependencies in turn, without
# building or copying the apps.
# Required tools must already be available. Only existing local feeds are
# registered; compilation may download sources missing from the SDK's dl/ cache.
#
# Exit status: 0 built, 2 SDK missing or not set up, 3 libraries or headers
# missing, 4 compile error, 1 any other failure.
set -u

repo=$(cd "$(dirname "$0")/.." && pwd)
prepare_only=
variants=
sdk_argument=
usage() {
	echo "usage: $0 [--prepare-only] [variant=base|mqtt|mqtt-ssl ...] NAME|DIRECTORY   (for example x86 or filogic)" >&2
	exit 1
}
for argument in "$@"; do
	case $argument in
	--prepare-only) [ -z "$prepare_only" ] || usage; prepare_only=1 ;;
	variant=base|variant=mqtt|variant=mqtt-ssl)
		case " $variants " in *" ${argument#variant=} "*) usage ;; esac
		variants="$variants ${argument#variant=}" ;;
	-*|*=*) usage ;;
	*) [ -z "$sdk_argument" ] && [ -n "$argument" ] || usage; sdk_argument=$argument ;;
	esac
done
[ -n "$sdk_argument" ] || usage
# Always in the same order, whatever the order of the arguments.
selected=
for variant in base mqtt mqtt-ssl; do
	case " ${variants:- base mqtt mqtt-ssl} " in *" $variant "*) selected="$selected $variant" ;; esac
done
. "$repo/tools/sdk-resolve.sh"
sdk_resolve "$sdk_argument"

fail() { # STATUS MESSAGE...
	status=$1
	shift
	printf '\nCANNOT BUILD (%s): ' "$name${variant:+, $variant}" >&2
	printf '%s\n' "$@" >&2
	exit "$status"
}

quiet() {
	[ "${VERBOSE:-}" != 1 ]
}

# Fails like fail, first showing the end of the log when make output was hidden.
fail_log() { # STATUS MESSAGE...
	if quiet; then
		printf '\nLast 30 lines of %s:\n' "$log" >&2
		tail -n 30 "$log" >&2
	fi
	fail "$@"
}

# Runs make with its output appended to the log, or shown with VERBOSE=1; the
# caller's tee also appends, so both writers keep to the end of the log.
sdk_make() { # MAKE-ARGUMENTS...
	if quiet; then
		make "$@" >>"$log" 2>&1
	else
		make "$@"
	fi
}

variant=
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
	if [ "$mqtt_variant" = ssl ]; then
		printf '%s\n' "$needed" | grep -Eq 'Shared library: \[libssl[.]' &&
			printf '%s\n' "$needed" | grep -Eq 'Shared library: \[libcrypto[.]'
	else
		! printf '%s\n' "$needed" | grep -Eq 'Shared library: \[lib(ssl|crypto)[.]'
	fi
}

. "$repo/tools/sdk-dependencies.sh"

# The newest PACKAGE file in the SDK's bin/, which is the one just built; the
# version follows the name, so cake-adapt never matches cake-adapt-mqtt.
newest_package() { # PACKAGE
	find "$sdk/bin/packages" \( -name "$1-[0-9]*.apk" -o -name "$1_[0-9]*.ipk" \) \
		-printf '%T@ %p\n' | sort -rn | head -n 1 | cut -d' ' -f2-
}

copy_package() { # SOURCE DESTINATION
	cp "$1" "$2"
	[ "$(sha256sum <"$1")" = "$(sha256sum <"$2")" ] ||
		fail 1 "the copy $2 does not match $1"
	sha256sum "$1"
	echo "copied to bin/$(basename "$2") (checksum verified)"
}

mkdir -p "$repo/build"
missing=
for variant in $selected; do
	case $variant in
	base) package=cake-adapt mqtt_variant= ;;
	mqtt) package=cake-adapt-mqtt mqtt_variant=nossl ;;
	mqtt-ssl) package=cake-adapt-mqtt-ssl mqtt_variant=ssl ;;
	esac
	log=$repo/build/sdk-$name-$variant.log
	status_file=$repo/build/sdk-$name-$variant.status
	contracts=$repo/build/sdk-$name-$variant.contracts
	plan=$repo/build/sdk-$name-$variant.plan
	if [ -n "$prepare_only" ]; then
		echo "Preparing $package dependencies in $sdk (log: build/sdk-$name-$variant.log)"
	else
		echo "Building $package in $sdk (log: build/sdk-$name-$variant.log)"
	fi
	: >"$log"
	{
		(prepare_dependencies)
		echo $? >"$status_file"
	} 2>&1 | tee -a "$log"
	status=$(cat "$status_file")
	rm -f "$status_file"
	[ "$status" -eq 0 ] || fail_log "$status" "dependency preparation failed (see $log)"
	if [ -n "$prepare_only" ]; then
		echo "READY ($name, $package): all daemon headers and libraries staged"
		continue
	fi
	selection="BUILD_VARIANT=$variant CONFIG_PACKAGE_$package=m"
	{
		# shellcheck disable=SC2086 # selection holds two words without spaces.
		sdk_make -C "$sdk" package/cake-adapt/clean NO_DEPS=1 $selection &&
			sdk_make -C "$sdk" package/cake-adapt/compile NO_DEPS=1 $selection
		echo $? >"$status_file"
	} 2>&1 | tee -a "$log"
	status=$(cat "$status_file")
	rm -f "$status_file"

	# Dependencies the SDK's package index lacks. Runtime-only ones (fping, bash)
	# do not stop the build: the package still records them, and the router
	# installs them from its own feeds.
	missing="$missing $(sed -n "s/.*Makefile 'package\/cake-adapt\/Makefile' has a dependency on '\([^']*\)', which does not exist.*/\1/p" "$log")"
	libraries=$(grep -A3 "Package $package is missing dependencies" "$log" | grep -v "Package $package" | tr -s ' \n' ' ')
	headers=$(sed -n 's/.*fatal error: \([^:]*\): No such file or directory.*/\1/p' "$log" | sort -u | tr '\n' ' ')

	if [ "$status" -eq 0 ] && [ -z "$libraries" ]; then
		echo
		built=$(newest_package "$package")
		[ -n "$built" ] || fail 1 "make succeeded but no $package package is in $sdk/bin/packages"
		# The SDK name goes before the extension: x86 and Filogic packages share a file name.
		file=$(basename "$built")
		mkdir -p "$repo/bin"
		echo "BUILT ($name, $package):"
		copy_package "$built" "$repo/bin/${file%.*}-$name.${file##*.}"
		continue
	fi
	[ -z "$libraries" ] ||
		fail_log 3 "the package is missing libraries: $libraries" \
			"Install their feed packages in the SDK and rebuild."
	[ -z "$headers" ] ||
		fail_log 3 "headers not found: $headers" \
			"A staged dependency disappeared or is incompatible; inspect the preflight plan and SDK feeds."
	errors=$(grep -E '(error|Error):' "$log" | grep -v -E '^make(\[[0-9]+\])?: \*\*\*|^ERROR: package/' | head -n 20)
	[ -z "$errors" ] ||
		fail_log 4 "compile errors:" "$errors"
	# Missing runtime packages (fping, bash) only warn, so they do not explain a
	# failed build; without V=s the compiler's own errors are not in the log.
	fail_log 1 "make exited with status $status, and the log shows no compiler error" \
		"See the compiler output with: make -C $sdk package/cake-adapt/compile NO_DEPS=1 $selection V=s"
done
variant=
if [ -n "$prepare_only" ]; then
	case $selected in
	*" mqtt mqtt-ssl"*) echo "Note: libmosquitto-ssl, prepared last, is the MQTT library left staged" ;;
	esac
	exit 0
fi

# The LuCI app, when the SDK links it; its package has no architecture.
luci_link=$(readlink -f "$sdk/package/luci-app-cake-adapt" 2>/dev/null)
if [ "$luci_link" = "$repo/luci-app-cake-adapt" ]; then
	log=$repo/build/sdk-$name-luci.log
	status_file=$repo/build/sdk-$name-luci.status
	echo "Building luci-app-cake-adapt in $sdk (log: build/sdk-$name-luci.log)"
	: >"$log"
	{
		sdk_make -C "$sdk" package/luci-app-cake-adapt/clean NO_DEPS=1 &&
			sdk_make -C "$sdk" package/luci-app-cake-adapt/compile NO_DEPS=1
		echo $? >"$status_file"
	} 2>&1 | tee -a "$log"
	[ "$(cat "$status_file")" -eq 0 ] || fail_log 1 "the luci-app-cake-adapt build failed (see $log)"
	rm -f "$status_file"
	built=$(newest_package luci-app-cake-adapt)
	[ -n "$built" ] || fail 1 "make succeeded but no luci-app-cake-adapt package is in $sdk/bin/packages"
	copy_package "$built" "$repo/bin/$(basename "$built")"
fi
missing=$(printf '%s\n' $missing | sort -u | tr '\n' ' ')
[ -z "${missing% }" ] ||
	printf 'warning: the SDK has no package for runtime dependencies: %s\n%s\n' "$missing" \
		"  (harmless for this build; to clear it: cd $sdk && ./scripts/feeds install $missing)"
exit 0
