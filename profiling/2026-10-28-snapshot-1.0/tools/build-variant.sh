#!/bin/sh
# build-variant.sh COMMIT|WORKTREE NAME: an x86_64 debug build of COMMIT, or of
# the working tree's src/ for WORKTREE, for the snapshot VM (2026-10-28 copy of
# the 2026-10-27 script), with the snapshot tree's own compile command
# (compile-line.txt, from make package/cake-adapt/compile V=s in ~/openwrt, the
# cake-adapt-mqtt variant) plus -g3 -gdwarf-4 -fno-omit-frame-pointer, from a
# git archive export. OPENWRT_SOURCE overrides ../openwrt. Its work files and
# output stay in the ignored build/variants/ of the repository, never here:
# bin/NAME and obj/NAME.o (the TCP filter of the same compile).
set -eu
commit=$1 name=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
sdk=${OPENWRT_SOURCE:-$repo/../openwrt}
out=$repo/build/variants
export_dir=$out/src-$name
rm -rf "$export_dir" && mkdir -p "$export_dir" "$out/bin" "$out/obj"
if [ "$commit" = WORKTREE ]; then
	tar -C "$repo" --exclude=src/build -cf - src | tar -x -C "$export_dir"
	version=$(sed -n 's/^PKG_VERSION:=//p' "$repo/Makefile")-r1
else
	git -C "$repo" archive "$commit" src | tar -x -C "$export_dir"
	version=$(git -C "$repo" show "$commit:Makefile" | sed -n 's/^PKG_VERSION:=//p')-r1
fi
line=$(cat "$here/compile-line.txt")
build_dir=$(printf '%s\n' "$line" | sed -n 's/^make -j1 -C \([^ ]*\) .*/\1/p')
# The package's build directory holds src/; the export's src/ stands in for it.
line=$(printf '%s\n' "$line" | sed -e "s|$build_dir|$export_dir/src|g" \
	-e 's|CFLAGS="-Os |CFLAGS="-g3 -gdwarf-4 -fno-omit-frame-pointer -Os |' \
	-e "s|VERSION=\"[^\"]*\"|VERSION=\"$version\"|")
toolchain=$(ls -d "$sdk"/staging_dir/toolchain-x86_64*)
export STAGING_DIR=$(ls -d "$sdk"/staging_dir/target-x86_64*)
export PATH="$toolchain/bin:$sdk/staging_dir/host/bin:$PATH"
eval "$line" > "$out/build-$name.log" 2>&1 || { tail -n 30 "$out/build-$name.log"; exit 1; }
cp "$export_dir/src/build/cake-adapt" "$out/bin/$name" 2>/dev/null || cp "$export_dir/src/cake-adapt" "$out/bin/$name"
cp "$export_dir/src/build/tcpdelay.bpf.o" "$out/obj/$name.o"
echo "$name $commit $version $(sha256sum "$out/bin/$name" | cut -c1-16)"
