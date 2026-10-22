#!/bin/sh
# build-variant.sh COMMIT NAME: a debug build of COMMIT for profiling, with the
# x86 SDK's own compile command (sdk-compile-line.txt, from
# make package/cake-adapt/compile V=s) plus -g3 -gdwarf-4
# -fno-omit-frame-pointer, from a git archive export. Output: bin/NAME and
# obj/NAME.o (the TCP filter of the same compile).
set -eu
commit=$1 name=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
sdk=${OPENWRT_SDK_X86:-$repo/../openwrt-sdk-x86}
export_dir=$here/src-$name
rm -rf "$export_dir" && mkdir -p "$export_dir" "$here/bin" "$here/obj"
git -C "$repo" archive "$commit" src | tar -x -C "$export_dir"
line=$(cat "$here/sdk-compile-line.txt")
build_dir=$(printf '%s\n' "$line" | sed -n 's/^make -j1 -C \([^ ]*\) .*/\1/p')
version=$(git -C "$repo" show "$commit:Makefile" | sed -n 's/^PKG_VERSION:=//p')-r1
# The package's build directory holds src/; the export's src/ stands in for it.
line=$(printf '%s\n' "$line" | sed -e "s|$build_dir|$export_dir/src|g" \
	-e 's|CFLAGS="-Os |CFLAGS="-g3 -gdwarf-4 -fno-omit-frame-pointer -Os |' \
	-e "s|VERSION=\"[^\"]*\"|VERSION=\"$version\"|")
toolchain=$(ls -d "$sdk"/staging_dir/toolchain-*)
export STAGING_DIR=$(ls -d "$sdk"/staging_dir/target-*)
export PATH="$toolchain/bin:$sdk/staging_dir/host/bin:$PATH"
eval "$line" > "$here/build-$name.log" 2>&1 || { tail -n 30 "$here/build-$name.log"; exit 1; }
cp "$export_dir/src/build/cake-adapt" "$here/bin/$name" 2>/dev/null || cp "$export_dir/src/cake-adapt" "$here/bin/$name"
cp "$export_dir/src/build/tcpdelay.bpf.o" "$here/obj/$name.o"
echo "$name $commit $version $(sha256sum "$here/bin/$name" | cut -c1-16)"
