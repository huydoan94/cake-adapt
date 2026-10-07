#!/bin/sh
# compile-commands.sh: writes compile_commands.json at the repository root for
# editors (clangd, the C/C++ extension), with the include paths of an OpenWrt
# x86 SDK, so they resolve libbpf's <bpf/...>, libnl-tiny and the kernel
# headers the socket filter uses.
#
# The SDK is $OPENWRT_SDK_X86, or ../openwrt-sdk-x86 beside the repository.
# Every C file under src/, tests/ and tools/ gets an entry. The socket filter
# (tcpdelay.bpf.c) is compiled as the package builds it: with the SDK's clang
# for the BPF target, the staged headers and the toolchain's kernel headers.
# The output holds absolute paths for this machine and is not committed.
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
sdk=${OPENWRT_SDK_X86:-$repo/../openwrt-sdk-x86}
[ -d "$sdk" ] || { echo "$0: no SDK at $sdk; set OPENWRT_SDK_X86" >&2; exit 1; }
sdk=$(cd "$sdk" && pwd)
staging=$(ls -d "$sdk"/staging_dir/target-*/usr/include | head -n 1)
toolchain=$(ls -d "$sdk"/staging_dir/toolchain-*/include | head -n 1)
bpf_clang=$sdk/staging_dir/host/llvm-bpf/bin/clang
libnl3=$(pkg-config --cflags-only-I libnl-3.0 2>/dev/null | sed 's/^-I//; s/ .*//')
libnl3=${libnl3:-/usr/include/libnl3}
warnings='"-Wall", "-Wextra", "-Wpedantic", "-Wformat=2", "-Wshadow", "-Wconversion"'

entry() { # FILE ARGUMENTS
	printf '%s\n  {\n    "directory": "%s",\n    "file": "%s",\n    "arguments": [%s, "-c", "%s"]\n  }' \
		"$separator" "$(dirname "$1")" "$1" "$2" "$1"
	separator=,
}

output=$repo/compile_commands.json
separator=
{
	printf '['
	find "$repo/src" "$repo/tests" "$repo/tools" -name '*.c' | sort | while read -r file; do
		case $file in
		*.bpf.c)
			entry "$file" "\"$bpf_clang\", \"-O2\", \"-g\", \"-target\", \"bpfel\", \"-Wall\", \"-Wextra\", \"-I$repo/src\", \"-I$repo/src/tcpdelay/include\", \"-I$staging\", \"-I$toolchain\""
			;;
		# The filter's host test builds it with GNU typeof, as tests/Makefile does.
		*/tests/tcpdelay/test_filter_accounting.c)
			entry "$file" "\"/usr/bin/gcc\", \"-std=gnu11\", \"-I$repo/src\", $warnings, \"-isystem\", \"$staging\""
			;;
		# The netlink tests use the host's libnl 3, which shares libnl-tiny's API.
		*/tests/platform/test_netlink.c | */tests/cake/test_cake.c)
			entry "$file" "\"/usr/bin/gcc\", \"-std=c11\", \"-I$repo/src\", $warnings, \"-isystem\", \"$staging\", \"-isystem\", \"$libnl3\""
			;;
		*)
			entry "$file" "\"/usr/bin/gcc\", \"-std=c11\", \"-I$repo/src\", $warnings, \"-isystem\", \"$staging\", \"-isystem\", \"$staging/libnl-tiny\""
			;;
		esac
	done
	printf '\n]\n'
} > "$output.tmp"
mv "$output.tmp" "$output"
echo "wrote $output ($(grep -c '"file"' "$output") files, SDK $sdk)"
