# sdk-resolve.sh: sourced by sdk-build.sh and host-test.sh.
#
# sdk_resolve NAME|DIRECTORY sets sdk (its directory), name (a label safe for
# file names) and variable (the environment variable that overrides it, empty
# for a directory). NAME picks $OPENWRT_SDK_<NAME> (upper case, other
# characters as _), else ../openwrt-sdk-<NAME> beside the repository: x86 is
# $OPENWRT_SDK_X86 or ../openwrt-sdk-x86, x86_64-snapshot is
# $OPENWRT_SDK_X86_64_SNAPSHOT or ../openwrt-sdk-x86_64-snapshot. An argument
# containing a slash is the SDK directory itself.
sdk_resolve() {
	case $1 in
	*/*)
		sdk=$1
		name=$(basename "$sdk" | sed 's/^openwrt-sdk-//')
		variable=
		;;
	*)
		name=$(printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_')
		variable=OPENWRT_SDK_$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')
		default=$repo/../openwrt-sdk-$name
		# The variable's name holds only A-Z, 0-9 and _, so eval expands nothing else.
		eval "sdk=\${$variable:-\$default}"
		;;
	esac
	name=$(printf '%s' "$name" | tr -c 'A-Za-z0-9._-' '_')
}

# sdk_check fails with status 2 unless sdk is an unpacked OpenWrt SDK; the
# caller defines fail STATUS MESSAGE...
sdk_check() {
	[ -f "$sdk/rules.mk" ] && [ -d "$sdk/staging_dir" ] ||
		fail 2 "no OpenWrt SDK at $sdk" \
			"Unpack the SDK there${variable:+, or set $variable to its directory}."
	sdk=$(cd "$sdk" && pwd)
}
