#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Fixed output textures and SDK-queried inputs; no desktop resizing involved.
# usage: bench-upscale.sh build | init|run quality|performance [width height]
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
cd "$R"
free -m
if [[ ${1:-} == build ]]; then
  x4r/tools/safe.sh x86_64-w64-mingw32-g++ -std=c++17 -O1 -static \
    -Ivendor/xess-sdk/inc x4r/src/bench-upscale.cpp vendor/xess-sdk/lib/libxess.lib \
    -ld3d12 -ldxgi -ldxguid -o x4r/build/bench-upscale.exe
  exit
fi
action=${1:?}; mode=${2:?}; width=${3:-2560}; height=${4:-1440}
[[ $action == init || $action == run ]] || exit 2
[[ $mode == quality || $mode == performance ]] || exit 2
dest="$R/quality/timing-${mode}-${width}x${height}"
mkdir -p "$dest"
exec 9>"$dest/.lock"; flock -n 9 || exit 2
cp x4r/build/bench-upscale.exe "$dest/"
for dll in d3d12.dll d3d12core.dll dxgi.dll; do
  cp "prefix/drive_c/$dll" "$dest/" 2>/dev/null || cp "prefix/drive_c/windows/system32/$dll" "$dest/"
done
cp prefix/drive_c/x4r_run/{libxess.dll,igdext64.dll,dxvk.conf} "$dest/"
export WINEPREFIX="$R/prefix" WINEDLLOVERRIDES='d3d12,d3d12core,dxgi=n;mscoree,mshtml=' WINEDEBUG=-all
export DXVK_CONFIG_FILE="${X4R_BENCH_CONFIG:-$dest/dxvk.conf}" DXVK_LOG_LEVEL=info VKD3D_DEBUG=warn
export INTC_ALT_DRIVER_EXTENSIONS_PATH="Z:${dest//\//\\}" IGDEXT_TRACE=1 IGDEXT_SKIP_SELFTEST=1
export VKD3D_SHADER_OVERRIDE="${X4R_BENCH_OVERRIDE:-$R/prefix/drive_c/x4r/ovr}"
cd "$dest"
# Audit every requested kernel before permitting a new preset to dispatch.
offset=$(stat -c %s "$R/prefix/drive_c/igdext_trace.log" 2>/dev/null || echo 0)
"$R/x4r/tools/safe.sh" timeout 180 wine ./bench-upscale.exe "$width" "$height" "$mode" init >init.log 2>&1
tail -c +$((offset+1)) "$R/prefix/drive_c/igdext_trace.log" >init-igdext.log
if grep -q DUMMY init-igdext.log || ! grep -q 'x4r translated kernel' init-igdext.log; then
  echo 'Missing translated kernels; dispatch skipped. Inspect init-igdext.log.' >&2
  exit 3
fi
grep '^mode=' init.log
if [[ $action == init ]]; then exit; fi
free -m
offset=$(stat -c %s "$R/prefix/drive_c/igdext_trace.log")
"$R/x4r/tools/safe.sh" timeout 180 wine ./bench-upscale.exe "$width" "$height" "$mode" >run.log 2>&1
tail -c +$((offset+1)) "$R/prefix/drive_c/igdext_trace.log" >igdext_trace.log
if grep -q DUMMY igdext_trace.log; then echo 'Invalid benchmark: dummy pipeline.' >&2; exit 3; fi
grep -E '^mode=|^GPU XeSS span:' run.log
grep 'GPU xess time per frame' igdext_trace.log | tail -1
