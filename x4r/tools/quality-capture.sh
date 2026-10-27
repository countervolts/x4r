#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Isolated motion/static replay. Does not install anything into a game.
# quality-capture.sh dp4a|dxil|wmma SCENE OUTPUT [motion]
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
mode=${1:?backend}; scene=$(realpath "${2:?scene}"); dest=$(realpath -m "${3:?output}"); motion=${4:-}
case "$mode" in dp4a|dxil|wmma) ;; *) echo 'backend must be dp4a, dxil or wmma' >&2; exit 2 ;; esac
mkdir -p "$dest"
[[ ! -e $dest/run.log ]] || { echo 'Use a new output directory to preserve captures' >&2; exit 2; }
cp "${X4R_QUALITY_EXE:-$R/x4r/build/quality.exe}" "$dest/quality.exe"
cp "$R/prefix/drive_c/x4r_run/libxess.dll" "$dest/"
for dll in d3d12.dll d3d12core.dll dxgi.dll; do
    cp "$R/prefix/drive_c/$dll" "$dest/" 2>/dev/null || cp "$R/prefix/drive_c/windows/system32/$dll" "$dest/"
done
export WINEPREFIX="$R/prefix" WINEDLLOVERRIDES='d3d12,d3d12core,dxgi=n;mscoree,mshtml=' WINEDEBUG=-all VKD3D_DEBUG=warn
unset VKD3D_SHADER_OVERRIDE INTC_ALT_DRIVER_EXTENSIONS_PATH IGDEXT_FORCE_FALLBACK X4R_CAPTURE X4R_SKIP X4R_SERIALIZE X4R_DUMP X4R_PROFILE X4R_DIR
if [[ $mode == dp4a ]]; then
    printf 'dxgi.hideAmdGpu = False\n' > "$dest/dxvk.conf"
else
    mkdir -p "$dest/x4r"
    cp "$R/private/third_party/xess-xmx-linux/shim/build/igdext64.dll" "$dest/"
    cp -r "${X4R_DXIL_DIR:-$R/prefix/drive_c/x4r/dxil}" "$dest/x4r/dxil"
    cp "$R/prefix/drive_c/x4r/splitz.txt" "$dest/x4r/"
    printf 'dxgi.customVendorId = 8086\ndxgi.customDeviceId = e20b\ndxgi.hideAmdGpu = False\n' > "$dest/dxvk.conf"
    printf 'Profile = dxil\nBackend = x4r\nCapture = false\n' > "$dest/x4r/x4r.ini"
    printf 'FrameSync = %s\n' "${X4R_QUALITY_FRAME_SYNC:-false}" >> "$dest/x4r/x4r.ini"
    export X4R_PROFILE=dxil INTC_ALT_DRIVER_EXTENSIONS_PATH="Z:${dest//\//\\}" X4R_DIR="Z:${dest//\//\\}\\x4r" IGDEXT_TRACE=1
    if [[ $mode == wmma ]]; then
        cp -r "${X4R_OVR_DIR:-$R/prefix/drive_c/x4r/ovr}" "$dest/x4r/ovr-rdna3"
        export X4R_PROFILE=rdna3 VKD3D_SHADER_OVERRIDE="Z:${dest//\//\\}\\x4r\\ovr-rdna3"
    fi
fi
export DXVK_CONFIG_FILE="$dest/dxvk.conf"
args=("Z:${scene//\//\\}" "Z:${dest//\//\\}")
[[ $motion != motion ]] || args+=(motion)
cd "$dest"
"$R/x4r/tools/safe.sh" timeout 180 wine ./quality.exe "${args[@]}" >run.log 2>&1
tail -3 run.log
