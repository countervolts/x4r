#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds the drag-in release zip (extract into the folder holding the game's .exe, then run x4r/install.sh once).
# usage: x4r/tools/package-release.sh [OUT_DIR]      (default dist/)
# Needs the local runtime from build-all.sh/build-fast.sh: private/prefix/drive_c/x4r/{dxil,ovr,ovr-rdna4,splitz.txt} and the built shim.
# The kernels are translations of Intel's XeSS kernels; whether they may be redistributed has not been established
# (THIRD_PARTY.md). Publishing them is the packager's call: set X4R_REDISTRIBUTE_KERNELS=1 to confirm it.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
[[ ${X4R_REDISTRIBUTE_KERNELS:-0} == 1 ]] || { echo 'Set X4R_REDISTRIBUTE_KERNELS=1 to confirm you may publish the translated kernels (see THIRD_PARTY.md).' >&2; exit 2; }
VERSION=$(cat "$R/packaging/VERSION")
OUT=$(realpath -m "${1:-$R/dist}")
NAME=x4r-$VERSION
STAGE=$OUT/$NAME
RT=$R/private/prefix/drive_c/x4r
SHIM=$R/private/third_party/xess-xmx-linux/shim
for f in "$SHIM/build/igdext64.dll" "$RT/splitz.txt" "$RT/dxil" "$RT/ovr" "$RT/ovr-rdna4"; do [[ -e $f ]] || { echo "missing $f" >&2; exit 2; }; done
n=$(wc -l < "$R/kernels/map.txt"); [[ $(ls "$RT/dxil" | wc -l) -eq $n ]] || { echo "dxil holds $(ls "$RT/dxil" | wc -l) kernels, map.txt lists $n" >&2; exit 2; }
[[ $(ls "$RT/ovr" | wc -l) -eq $(ls "$RT/ovr-rdna4" | wc -l) ]] || { echo 'ovr and ovr-rdna4 differ in size' >&2; exit 2; }
# the shipped patch must reproduce the shipped shim's source
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
git -C "$R/private/third_party/xess-xmx-linux" archive c8d481cf7a46b86645e2ffa12f665840b1665980 shim | tar -x -C "$tmp"
(cd "$tmp/shim" && git init -q . && git apply "$R/patches/igdext-x4r.patch")
for f in X4r.cpp D3D12Api.cpp; do cmp "$tmp/shim/src/dll/$f" "$SHIM/src/dll/$f" || { echo "patches/igdext-x4r.patch is stale ($f)" >&2; exit 2; }; done

rm -rf "$STAGE"; mkdir -p "$STAGE/x4r/licenses" "$STAGE/x4r/source"
cp "$SHIM/build/igdext64.dll" "$STAGE/"
cp -r "$RT/dxil" "$STAGE/x4r/dxil"
cp -r "$RT/ovr" "$STAGE/x4r/ovr-rdna3"
cp -r "$RT/ovr-rdna4" "$STAGE/x4r/ovr-rdna4"
cp "$RT/splitz.txt" "$STAGE/x4r/"
cp "$R/packaging/"{x4r.ini,README.txt,install.sh,uninstall.sh,x4r-check.sh,VERSION} "$STAGE/x4r/"
chmod +x "$STAGE/x4r/"*.sh
cp "$R/LICENSE" "$STAGE/x4r/licenses/x4r-LICENSE-GPL-3.0.txt"
cp "$R/LICENSES/"* "$STAGE/x4r/licenses/"
cp "$R/THIRD_PARTY.md" "$STAGE/x4r/licenses/"
cp "$R/patches/igdext-x4r.patch" "$STAGE/x4r/source/"
cat > "$STAGE/x4r/source/SOURCES.txt" <<SRC
x4r $VERSION  https://github.com/countervolts/x4r  (GPL-3.0-only)
igdext64.dll = xess-xmx-linux commit c8d481cf7a46b86645e2ffa12f665840b1665980 (https://github.com/HHHHanasak1/xess-xmx-linux, MIT)
              + igdext-x4r.patch in this folder, built with MinGW-w64 (static), CMake Release.
Kernels (dxil/, ovr-*): produced by x4r/src/x4rwave.cpp + DirectXShaderCompiler from the XeSS 2.0.2.68 kernels;
              recipe in docs/DEVELOPMENT.md (x4r/tools/build-fast.sh).
SRC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$R" log -1 --format=%ct 2>/dev/null || date +%s)}
find "$STAGE" -exec touch -d "@$SOURCE_DATE_EPOCH" {} +
rm -f "$OUT/$NAME.zip" "$OUT/$NAME.zip.sha256"
(cd "$STAGE" && find . -type f | LC_ALL=C sort | sed 's|^\./||' | zip -X -q -9 "$OUT/$NAME.zip" -@)
(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")
unzip -l "$OUT/$NAME.zip" | tail -1; cat "$OUT/$NAME.zip.sha256"
