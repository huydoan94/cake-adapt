# Project-specific OpenWrt 25.12 staging contracts, in build dependency order.
# Sourced by sdk-build.sh, which sets variant, package and mqtt_variant (empty
# for the base variant, else nossl or ssl). No recursive dependency or
# toolchain builds.
sdk_dependencies() {
	cat <<'DEPENDENCIES'
liblua|lua|base|utils/lua|lua|usr/include/lua.h usr/include/lauxlib.h usr/lib/liblua.so
libjson-c|libjson-c|base|libs/libjson-c|libjson-c|usr/include/json-c/json.h usr/lib/pkgconfig/json-c.pc usr/lib/libjson-c.so
libubox|libubox|base|libs/libubox|libubox|usr/include/libubox/uloop.h usr/include/libubox/ustream.h usr/include/libubox/utils.h usr/include/libubox/blobmsg.h usr/include/libubox/blobmsg_json.h usr/lib/libubox.so usr/lib/libblobmsg_json.so
libuci|uci|base|system/uci|uci|usr/include/uci.h usr/lib/libuci.so
libubus|ubus|base|system/ubus|ubus|usr/include/libubus.h usr/lib/libubus.so
libnl-tiny|libnl-tiny|base|libs/libnl-tiny|libnl-tiny|usr/include/libnl-tiny/netlink/netlink.h usr/include/libnl-tiny/netlink/attr.h usr/include/libnl-tiny/netlink/errno.h usr/include/libnl-tiny/netlink/handlers.h usr/include/libnl-tiny/netlink/msg.h usr/include/libnl-tiny/netlink/socket.h usr/lib/libnl-tiny.so
zlib|zlib|base|libs/zlib|zlib|usr/include/zlib.h usr/include/zconf.h usr/lib/pkgconfig/zlib.pc usr/lib/libz.so
DEPENDENCIES
	if ! grep -q '^CONFIG_USE_GLIBC=y$' "$sdk/.config"; then
		printf '%s\n' 'argp-standalone|argp-standalone|base|libs/argp-standalone|argp-standalone|usr/include/argp.h usr/lib/libargp.a'
	fi
	if grep -q '^CONFIG_USE_MUSL=y$' "$sdk/.config" || [ "${toolchain##*_}" = musl ]; then
		printf '%s\n' 'musl-fts|musl-fts|base|libs/musl-fts|musl-fts|usr/include/fts.h usr/lib/libfts.so'
	fi
	cat <<'DEPENDENCIES'
libelf|elfutils|base|libs/elfutils|elfutils|usr/include/libelf.h usr/include/gelf.h usr/lib/pkgconfig/libelf.pc usr/lib/libelf.so
libbpf|libbpf|base|libs/libbpf|libbpf|usr/include/bpf/libbpf.h usr/include/bpf/bpf.h usr/include/bpf/bpf_helpers.h usr/include/bpf/bpf_endian.h usr/lib/libbpf.so
DEPENDENCIES
	# The base variant links no MQTT library.
	[ -n "$mqtt_variant" ] || return 0
	printf '%s\n' 'libcares|c-ares|packages|libs/c-ares|c-ares|usr/include/ares.h usr/lib/libcares.so'
	if [ "$mqtt_variant" = ssl ]; then
		printf '%s\n' 'libopenssl|openssl|base|libs/openssl|openssl|usr/include/openssl/ssl.h usr/include/openssl/crypto.h usr/lib/libssl.so usr/lib/libcrypto.so usr/lib/pkgconfig/openssl.pc usr/lib/pkgconfig/libssl.pc usr/lib/pkgconfig/libcrypto.pc'
	fi
	printf '%s\n' "libmosquitto-$mqtt_variant|mosquitto|packages|net/mosquitto|mosquitto.$mqtt_variant|usr/include/mosquitto.h usr/lib/libmosquitto.so usr/lib/libmosquitto.so.1"
}

sdk_elf_signature() { # PATH: class, byte order and machine from a readable ELF.
	elf_header=$(LC_ALL=C readelf -h "$1" 2>/dev/null) || return 1
	printf '%s\n' "$elf_header" | grep -q 'Type:.*DYN' || return 1
	printf '%s\n' "$elf_header" | sed -n '/Class:/p; /Data:/p; /Machine:/p'
}

sdk_dependency_ready() {
	[ -f "$staging/stamp/.$stamp_installed" ] || return 1
	for artifact in $artifacts; do
		[ -f "$staging/$artifact" ] || return 1
		case $artifact in
		*.so|*.so.*)
			library_elf=$(sdk_elf_signature "$staging/$artifact") || return 1
			[ "$library_elf" = "$target_elf" ] || return 1 ;;
		esac
	done
	[ "$source" != mosquitto ] || mqtt_staged
}

sdk_dependency_target() {
	if [ -f "$sdk/package/$source/Makefile" ]; then
		target=package/$source
		register=
	elif [ -f "$sdk/package/feeds/$feed/$source/Makefile" ]; then
		target=package/feeds/$feed/$source
		register=
	else
		[ -f "$sdk/feeds/$feed/$local_path/Makefile" ] ||
			fail 3 "the local $feed feed lacks $dependency ($local_path); prepare that feed first"
		[ -x "$sdk/scripts/feeds" ] || fail 3 "missing executable $sdk/scripts/feeds"
		target=package/feeds/$feed/$source
		register=1
	fi
}

sdk_tool_check() { # EXECUTABLE DESCRIPTION [ARGUMENTS...]
	tool=$1
	description=$2
	shift 2
	# SDK wrappers (notably pkg-config) rely on the same staging environment
	# that package.mk exports for configure and compile.
	[ -x "$tool" ] && STAGING_DIR="$staging" STAGING_DIR_HOST="$sdk/staging_dir/host" \
		STAGING_DIR_HOSTPKG="$sdk/staging_dir/hostpkg" STAGING_PREFIX="$staging/usr" \
		PKG_CONFIG_LIBDIR="$staging/usr/lib/pkgconfig" "$tool" "$@" >/dev/null 2>&1 ||
		fail 2 "missing or unusable $description: $tool; prepare the SDK tool first"
}

sdk_toolchain_check() {
	set -- "$sdk"/staging_dir/toolchain-*
	[ "$#" -eq 1 ] && [ -d "$1" ] || fail 2 "expected one staged toolchain in $sdk"
	toolchain=$1
	gcc=
	for candidate in "$toolchain"/bin/*-gcc; do
		[ -x "$candidate" ] || continue
		resolved=$(readlink -f "$candidate")
		[ -z "$gcc" ] || [ "$gcc" = "$resolved" ] ||
			fail 2 "ambiguous target gcc binaries in $toolchain/bin"
		gcc=$resolved
	done
	[ -n "$gcc" ] || fail 2 "target gcc is missing from $toolchain/bin"
	sdk_tool_check "$gcc" 'target gcc' --version
	# libc may be a linker script on glibc; its runtime soname or libgcc is ELF.
	target_elf=
	for reference in "$toolchain/lib/libc.so" "$toolchain/lib/libc.so.6" "$toolchain/lib/libgcc_s.so.1"; do
		if target_elf=$(sdk_elf_signature "$reference"); then
			break
		fi
	done
	[ -n "$target_elf" ] || fail 2 "cannot read a target runtime ELF in $toolchain/lib"
	for header in stdio.h linux/types.h linux/bpf.h asm/types.h; do
		[ -f "$toolchain/include/$header" ] ||
			fail 2 "missing staged libc/Linux UAPI header: $toolchain/include/$header"
	done
	printf '#include <stdio.h>\n#include <linux/bpf.h>\n' |
		"$gcc" -x c -fsyntax-only - >/dev/null 2>&1 ||
		fail 2 "target gcc cannot use its staged libc/Linux UAPI headers"
	if grep -q '^CONFIG_USE_LLVM_HOST=y$' "$sdk/.config"; then
		llvm_host=$(sed -n 's/^CONFIG_BPF_TOOLCHAIN_HOST_PATH="\(.*\)"$/\1/p' "$sdk/.config")
		llvm_path=${llvm_host:+$llvm_host/bin:}$PATH
		clang=$(PATH=$llvm_path command -v clang || PATH=$llvm_path command -v clang-13 ||
			PATH=$llvm_path command -v clang-12) || fail 2 "host BPF clang is missing"
		llvm_suffix=$(basename "$clang" | sed 's/^clang//')
		strip=$(PATH="$(dirname "$clang"):$llvm_path" command -v "llvm-strip$llvm_suffix") ||
			fail 2 "host BPF llvm-strip$llvm_suffix is missing"
	elif grep -q '^CONFIG_USE_LLVM_BUILD=y$' "$sdk/.config"; then
		clang=$sdk/staging_dir/host/llvm-bpf/bin/clang
		strip=$sdk/staging_dir/host/llvm-bpf/bin/llvm-strip
	elif grep -q '^CONFIG_USE_LLVM_PREBUILT=y$' "$sdk/.config"; then
		clang=$sdk/llvm-bpf/bin/clang
		strip=$sdk/llvm-bpf/bin/llvm-strip
	else
		fail 2 "no supported CONFIG_USE_LLVM_HOST/BUILD/PREBUILT BPF compiler is selected"
	fi
	sdk_tool_check "$strip" 'BPF llvm-strip' --version
	bpf_target=bpfel
	grep -q '^CONFIG_BIG_ENDIAN=y$' "$sdk/.config" && bpf_target=bpfeb
	clang_major=$("$clang" --target="$bpf_target" -dM -E - </dev/null 2>/dev/null |
		sed -n 's/^#define __clang_major__ //p')
	case $clang_major in ''|*[!0-9]*) fail 2 "BPF clang cannot preprocess for $bpf_target: $clang";; esac
	[ "$clang_major" -ge 12 ] || fail 2 "BPF clang 12 or newer is required: $clang"
	printf 'int ready(void) { return 0; }\n' |
		"$clang" -target "$bpf_target" -x c -c -o /dev/null - >/dev/null 2>&1 ||
		fail 2 "BPF clang cannot generate $bpf_target objects: $clang"
}

sdk_luci_tool() { # NAME HOST-TARGET
	luci_tool=
	for location in "$sdk/staging_dir/hostpkg/bin" "$sdk/staging_dir/host/bin" "$sdk/scripts"; do
		if [ -x "$location/$1" ]; then
			luci_tool=$location/$1
			break
		fi
	done
	[ -n "$luci_tool" ] || luci_tool=$(command -v "$1") || {
		if [ "$1" = po2lmo ]; then
			fail 2 "LuCI translations need $1; prepare $2 first"
		fi
		echo "warning: LuCI $1 is unavailable; luci.mk keeps original assets (prepare $2 for minification)"
		return 0
	}
	# Exercise the tools with empty or minimal input rather than trusting a link.
	case $1 in
	jsmin) printf 'var ready = 1;\n' | "$luci_tool" >/dev/null 2>&1 ;;
	csstidy) "$luci_tool" --version >/dev/null 2>&1 ;;
	luasrcdiet) "$luci_tool" --version >/dev/null 2>&1 ;;
	po2lmo) "$luci_tool" >/dev/null 2>&1; [ "$?" -eq 1 ] ;;
	esac || {
		[ "$1" != po2lmo ] || fail 2 "LuCI tool $luci_tool is unusable; prepare $2 first"
		echo "warning: LuCI $luci_tool is unusable; luci.mk keeps original assets"
	}
}

sdk_luci_check() {
	luci_link=$(readlink -f "$sdk/package/luci-app-cake-adapt" 2>/dev/null)
	[ "$luci_link" = "$repo/luci-app-cake-adapt" ] || return 0
	[ -f "$sdk/feeds/luci/luci.mk" ] || fail 2 "linked LuCI app needs $sdk/feeds/luci/luci.mk"
	luci=$repo/luci-app-cake-adapt
	if grep -q '^CONFIG_LUCI_JSMIN=y$' "$sdk/.config" &&
		[ -n "$(find "$luci/htdocs" -type f -name '*.js' 2>/dev/null)" ]; then
		sdk_luci_tool jsmin package/feeds/luci/luci-base/host/compile
	fi
	if grep -q '^CONFIG_LUCI_CSSTIDY=y$' "$sdk/.config" &&
		[ -n "$(find "$luci/htdocs" -type f -name '*.css' 2>/dev/null)" ]; then
		sdk_luci_tool csstidy package/feeds/packages/csstidy/host/compile
	fi
	if grep -q '^CONFIG_LUCI_SRCDIET=y$' "$sdk/.config" &&
		[ -n "$(find "$luci/luasrc" -type f -name '*.lua' 2>/dev/null)" ]; then
		sdk_luci_tool luasrcdiet package/feeds/packages/luasrcdiet/host/compile
	fi
	# luci.mk only builds known, selected translation languages.
	for translations in "$luci"/po/*; do
		[ -d "$translations" ] || continue
		language=${translations##*/}
		grep -q "^LUCI_LANG[.]$language=" "$sdk/feeds/luci/luci.mk" || continue
		if grep -Eq "^CONFIG_(ALL=y|LUCI_LANG_$language=[my])$" "$sdk/.config" &&
			[ -n "$(find "$translations" -type f -name '*.po')" ]; then
			sdk_luci_tool po2lmo package/feeds/luci/luci-base/host/compile
			break
		fi
	done
}

sdk_dependencies_preflight() {
	# This is an explicit contract, not a Make/Kconfig dependency resolver.
	# Fail on new direct dependencies until their build/runtime role is recorded.
	# Every variant takes the common DEPENDS and adds its own.
	declared=$(sed -n '/^define Package\/cake-adapt\/Default$/,/^endef$/p' "$repo/Makefile" |
		sed -n 's/^[[:space:]]*DEPENDS[[:space:]]*[+:]*=[[:space:]]*//p')
	[ -n "$declared" ] || fail 3 "could not read cake-adapt's dependency contract"
	own=$(sed -n "/^define Package\/$package\$/,/^endef\$/p" "$repo/Makefile")
	printf '%s\n' "$own" | grep -q '^\$(call Package/cake-adapt/Default)$' ||
		fail 3 "could not read the $package package's dependency contract"
	own=$(printf '%s\n' "$own" | sed -n 's/^[[:space:]]*DEPENDS[[:space:]]*[+:]*=[[:space:]]*//p')
	for dependency in $declared $own; do
		case $dependency in
		+libuci|+libubox|+libubus|+libnl-tiny|+libbpf|+zlib|+libmosquitto-nossl|+libmosquitto-ssl|+ca-bundle|+fping|+bash|'$(BPF_DEPENDS)') ;;
		*) fail 3 "unrepresented cake-adapt dependency: $dependency; update tools/sdk-dependencies.sh" ;;
		esac
	done
	# The declared MQTT library is the one this variant is built against.
	expected=${mqtt_variant:++libmosquitto-$mqtt_variant}${mqtt_variant:+ }
	[ "$mqtt_variant" != ssl ] || expected="$expected+ca-bundle "
	actual=
	for dependency in $declared $own; do
		case $dependency in +libmosquitto-*|+ca-bundle) actual="$actual$dependency " ;; esac
	done
	[ "$actual" = "$expected" ] ||
		fail 3 "$package declares '${actual% }' but its variant needs '${expected% }'"
	command -v readelf >/dev/null || fail 2 "readelf is needed to verify the staged MQTT library"
	set -- "$sdk"/staging_dir/target-*
	[ "$#" -eq 1 ] && [ -d "$1" ] || fail 2 "expected one target staging directory in $sdk"
	staging=$1
	sdk_toolchain_check
	# OpenSSL requires libatomic on these targets; it must come from the
	# existing toolchain, never from a recursive toolchain build.
	if [ "$mqtt_variant" = ssl ] && grep -Eq '^CONFIG_(arm|armeb|mips|mipsel|powerpc|arc)=y$' "$sdk/.config"; then
		atomic_elf=$(sdk_elf_signature "$toolchain/lib/libatomic.so.1") ||
			fail 2 "OpenSSL needs a staged toolchain libatomic.so.1 on this target"
		[ "$atomic_elf" = "$target_elf" ] || fail 2 "the toolchain libatomic.so.1 has the wrong target"
	fi
	sdk_luci_check
	sdk_dependencies >"$contracts"
	declared_build=$(sed -n 's/^[[:space:]]*PKG_BUILD_DEPENDS[[:space:]]*[+:?]*=[[:space:]]*//p' "$repo/Makefile")
	represented=$(cut -d'|' -f1,2 "$contracts" | tr '|\n' '  ')
	for dependency in $declared_build; do
		case " $represented " in
		*" $dependency "*) ;;
		*) fail 3 "unrepresented cake-adapt build dependency: $dependency; update tools/sdk-dependencies.sh" ;;
		esac
	done
	: >"$plan"
	cmake_needed=
	autotools_needed=
	pkg_config_needed=
	perl_needed=
	while IFS='|' read -r dependency source feed local_path stamp artifacts; do
		stamp_installed=${stamp}_installed
		sdk_dependency_target
		action=ready
		sdk_dependency_ready || action=stage
		printf '%s: %s%s\n' "$dependency" "$action" "${register:+ (register local $feed feed)}"
		printf '%s|%s|%s|%s|%s|%s|%s|%s\n' "$dependency" "$source" "$feed" "$stamp" "$artifacts" "$target" "$register" "$action" >>"$plan"
		if [ "$action" = stage ]; then
			case $source in
			libjson-c|libubox|uci|ubus|libnl-tiny|zlib|c-ares|mosquitto) cmake_needed=1 ;;
			esac
			case $source in elfutils|musl-fts) autotools_needed=1 ;; esac
			case $source in openssl) perl_needed=1 ;; esac
			case $source in lua|argp-standalone) ;; *) pkg_config_needed=1 ;; esac
		fi
	done <"$contracts"
	if [ -n "$cmake_needed" ]; then
		sdk_tool_check "$sdk/staging_dir/host/bin/cmake" 'CMake (tools/cmake)' --version
		sdk_tool_check "$sdk/staging_dir/host/bin/ninja" 'Ninja (tools/ninja)' --version
	fi
	if [ -n "$autotools_needed" ]; then
		for autotool in autoreconf autoconf automake aclocal autoheader libtoolize autom4te m4; do
			sdk_tool_check "$sdk/staging_dir/host/bin/$autotool" "Autotools $autotool" --version
		done
	fi
	[ -z "$pkg_config_needed" ] ||
		sdk_tool_check "$sdk/staging_dir/host/bin/pkg-config" 'pkg-config (tools/pkgconf)' --version
	if [ -n "$perl_needed" ]; then
		perl_tool=$sdk/staging_dir/host/bin/perl
		[ -x "$perl_tool" ] || perl_tool=$(command -v perl) || fail 2 "OpenSSL Configure needs Perl"
		sdk_tool_check "$perl_tool" 'Perl for OpenSSL Configure' -e 'exit 0'
	fi
	echo 'Preflight complete; all required package recipes and build tools are available.'
}

sdk_dependency_build() {
	# CONFIG_ALL in an SDK must not package unrelated variants/tools. Each source
	# has a bounded set of outputs; optional Lua modules build against liblua.
	set -- NO_DEPS=1 CONFIG_ALL= CONFIG_ALL_NONSHARED= CONFIG_BUILD_NLS=
	case $source in
	lua) set -- "$@" CONFIG_PACKAGE_liblua=m CONFIG_PACKAGE_lua= CONFIG_PACKAGE_luac= CONFIG_PACKAGE_lua-examples= ;;
	libubox) set -- "$@" CONFIG_PACKAGE_libubox=m CONFIG_PACKAGE_libblobmsg-json=m CONFIG_PACKAGE_libjson-script= CONFIG_PACKAGE_jshn= CONFIG_PACKAGE_libubox-lua= ;;
	uci) set -- "$@" CONFIG_PACKAGE_libuci=m CONFIG_PACKAGE_uci= CONFIG_PACKAGE_libuci-lua= ;;
	ubus) set -- "$@" CONFIG_PACKAGE_libubus=m CONFIG_PACKAGE_ubus= CONFIG_PACKAGE_ubusd= CONFIG_PACKAGE_libubus-lua= ;;
	zlib) set -- "$@" CONFIG_PACKAGE_zlib=m CONFIG_PACKAGE_zlib-dev= ;;
	elfutils) set -- "$@" CONFIG_PACKAGE_libelf=m CONFIG_PACKAGE_libdw= CONFIG_PACKAGE_libasm= ;;
	openssl)
		set -- "$@" CONFIG_PACKAGE_libopenssl=m CONFIG_PACKAGE_libopenssl-afalg= \
			CONFIG_PACKAGE_libopenssl-devcrypto= CONFIG_PACKAGE_libopenssl-padlock= \
			CONFIG_PACKAGE_libopenssl-legacy= CONFIG_PACKAGE_libopenssl-conf= \
			CONFIG_PACKAGE_openssl-util= CONFIG_OPENSSL_ENGINE_BUILTIN= \
			CONFIG_OPENSSL_ENGINE_BUILTIN_AFALG= CONFIG_OPENSSL_ENGINE_BUILTIN_DEVCRYPTO= \
			CONFIG_OPENSSL_ENGINE_BUILTIN_PADLOCK= ;;
	mosquitto)
		other_variant=ssl
		[ "$mqtt_variant" != ssl ] || other_variant=nossl
		set -- "$@" "BUILD_VARIANT=$mqtt_variant" "CONFIG_PACKAGE_libmosquitto-$mqtt_variant=m" \
			"CONFIG_PACKAGE_libmosquitto-$other_variant=" CONFIG_PACKAGE_libmosquittopp= \
			CONFIG_PACKAGE_mosquitto-ssl= CONFIG_PACKAGE_mosquitto-nossl= \
			CONFIG_PACKAGE_mosquitto-client-ssl= CONFIG_PACKAGE_mosquitto-client-nossl= \
			CONFIG_MOSQUITTO_LWS= CONFIG_MOSQUITTO_DYNAMIC_SECURITY= \
			CONFIG_MOSQUITTO_CTRL= CONFIG_MOSQUITTO_PASSWD= ;;
	*) set -- "$@" "CONFIG_PACKAGE_$dependency=m" ;;
	esac
	echo "Staging $dependency ($target)"
	make -C "$sdk" "$target/clean" "$@" && make -C "$sdk" "$target/compile" "$@" ||
		fail 3 "could not stage $dependency"
}

prepare_dependencies() {
	sdk_dependencies_preflight
	while IFS='|' read -r dependency source feed stamp artifacts target register action; do
		if [ -n "$register" ]; then
			(cd "$sdk" && ./scripts/feeds install -p "$feed" "$dependency") ||
				fail 3 "could not register $dependency from the local $feed feed"
			[ -f "$sdk/$target/Makefile" ] || fail 3 "feed registration did not create $target"
		fi
		stamp_installed=${stamp}_installed
		if [ "$action" = stage ]; then
			sdk_dependency_build
		fi
		sdk_dependency_ready || fail 3 "the prepared $dependency stamp, headers or libraries are missing or invalid"
	done <"$plan"
}
