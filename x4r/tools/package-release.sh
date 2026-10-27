#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds the drag-in release zip: extract into the folder holding the game's .exe and launch. OptiScaler (dxgi.dll) loads
# x4r/x4r.asi, which installs the shim into the Proton prefix on first start.
# usage: X4R_OPTISCALER=<OptiScaler 0.9.x .7z/.zip or extracted folder> x4r/tools/package-release.sh [OUT_DIR]   (default dist/)
# Needs the local runtime from build-all.sh/build-fast.sh: private/prefix/drive_c/x4r/{dxil,ovr,ovr-rdna4,splitz.txt} and the built shim.
# The kernels are translations of Intel's XeSS kernels; whether they may be redistributed has not been established
# (THIRD_PARTY.md). Publishing them is the packager's call: set X4R_REDISTRIBUTE_KERNELS=1 to confirm it.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
[[ ${X4R_REDISTRIBUTE_KERNELS:-0} == 1 ]] || { echo 'Set X4R_REDISTRIBUTE_KERNELS=1 to confirm you may publish the translated kernels (see THIRD_PARTY.md).' >&2; exit 2; }
OPTI=${X4R_OPTISCALER:?set X4R_OPTISCALER to the OptiScaler release archive or extracted folder}
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

tmp=${tmp:-$(mktemp -d)}
if [[ -d $OPTI ]]; then OPTX=$OPTI; else
  OPTX=$tmp/optiscaler; mkdir -p "$OPTX"
  case $OPTI in *.7z) 7z x -y -o"$OPTX" "$OPTI" >/dev/null ;; *.zip) unzip -q -o "$OPTI" -d "$OPTX" ;; *) echo 'X4R_OPTISCALER must be .7z, .zip or a folder' >&2; exit 2 ;; esac
fi
for f in OptiScaler.dll OptiScaler.ini libxess.dll Licenses/XeSS_LICENSE.txt; do [[ -f $OPTX/$f ]] || { echo "OptiScaler bundle lacks $f" >&2; exit 2; }; done
# the kernels were translated from this exact XeSS build
[[ $(sha256sum "$OPTX/libxess.dll" | cut -d' ' -f1) == 251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7 ]] || { echo 'libxess.dll is not XeSS 2.0.2.68 (kernels would not match)' >&2; exit 2; }

rm -rf "$STAGE"; mkdir -p "$STAGE/x4r/licenses" "$STAGE/x4r/source"
cp "$OPTX/OptiScaler.dll" "$STAGE/dxgi.dll"
cp "$OPTX/libxess.dll" "$STAGE/"
cp -r "$OPTX/Licenses" "$STAGE/Licenses"
# OptiScaler: XeSS as the upscaler, ASI plugins on (loaded from x4r/), no GPU spoofing of its own (x4r gives the game an Intel id via dxvk.conf,
# so XeSS keeps its XMX path and the game offers FSR 3.1 / XeSS)
python3 - "$OPTX/OptiScaler.ini" "$STAGE/OptiScaler.ini" <<'PY'
import re,sys
src,dst=sys.argv[1:]
t=open(src,encoding='utf-8',errors='surrogateescape').read()
for key,val in (('Dx12Upscaler','xess'),('LoadAsiPlugins','true'),('Dxgi','false'),('Path','x4r')):
    t,n=re.subn(r'(?m)^'+key+r'=.*$',key+'='+val,t)
    if n!=1: raise SystemExit(f'OptiScaler.ini: {key} matched {n} times')
open(dst,'w',encoding='utf-8',errors='surrogateescape').write(t)
PY
X=$STAGE/x4r
cp "$SHIM/build/igdext64.dll" "$X/x4r.asi"
cp -r "$RT/dxil" "$X/dxil"
cp -r "$RT/ovr" "$X/ovr-rdna3"
cp -r "$RT/ovr-rdna4" "$X/ovr-rdna4"
cp "$RT/splitz.txt" "$X/"
cp "$R/packaging/"{x4r.ini,x4r-check.sh,uninstall.sh,VERSION} "$X/"
cp "$R/packaging/README.txt" "$STAGE/X4R_README.txt"
cp "$R/docs/GHOSTING_AUDIT.md" "$STAGE/X4R_GHOSTING_AUDIT.md"
cp "$R/docs/FRAME_SYNC_TRIAL.md" "$STAGE/X4R_FRAME_SYNC_TRIAL.md"
chmod +x "$X/"*.sh
cp "$R/LICENSE" "$X/licenses/x4r-LICENSE-GPL-3.0.txt"
cp "$R/LICENSES/"* "$X/licenses/"
cp "$R/THIRD_PARTY.md" "$X/licenses/"
cp "$R/patches/igdext-x4r.patch" "$X/source/"
mkdir -p "$X/source/x4r" "$X/source/docs"
cp -r "$R/x4r/src" "$R/x4r/tools" "$X/source/x4r/"
cp "$R/docs/DEVELOPMENT.md" "$R/docs/GHOSTING_AUDIT.md" "$X/source/docs/"
cp "$R/docs/FRAME_SYNC_TRIAL.md" "$X/source/docs/"
cat > "$X/source/SOURCES.txt" <<SRC
x4r $VERSION  https://github.com/countervolts/x4r  (GPL-3.0-only)
x4r.asi (and the igdext64.dll it installs) = xess-xmx-linux commit c8d481cf7a46b86645e2ffa12f665840b1665980
              (https://github.com/HHHHanasak1/xess-xmx-linux, MIT) + igdext-x4r.patch in this folder, MinGW-w64, static.
dxgi.dll = OptiScaler.dll from $(basename "$OPTI") (https://github.com/optiscaler/OptiScaler, GPL-3.0), unmodified.
libxess.dll = Intel XeSS 2.0.2.68 as shipped in that OptiScaler archive, unmodified (Licenses/XeSS_LICENSE.txt).
Kernels (dxil/, ovr-*): produced by x4r/src/x4rwave.cpp + DirectXShaderCompiler from XeSS 2.0.2.68;
              recipe in docs/DEVELOPMENT.md (x4r/tools/build-fast.sh).
SRC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$R" log -1 --format=%ct 2>/dev/null || date +%s)}
find "$STAGE" -exec touch -d "@$SOURCE_DATE_EPOCH" {} +
rm -f "$OUT/$NAME.zip" "$OUT/$NAME.zip.sha256"
(cd "$STAGE" && find . -type f | LC_ALL=C sort | sed 's|^\./||' | zip -X -q -9 "$OUT/$NAME.zip" -@)
(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")
unzip -l "$OUT/$NAME.zip" | tail -1; cat "$OUT/$NAME.zip.sha256"
