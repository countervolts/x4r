#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Probe: run Intel's XeSS DX12 sample on AMD with an Intel vendor ID and the igdext shim, dump CM kernels.
source "$(dirname -- "${BASH_SOURCE[0]}")/x4r/tools/common.sh"
export WINEPREFIX="${X4R_PREFIX:-$X4R_ROOT/prefix}"
R=$WINEPREFIX/drive_c/x4r_run
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n;mscoree,mshtml="
export DXVK_CONFIG_FILE=$R/dxvk.conf
export INTC_ALT_DRIVER_EXTENSIONS_PATH='C:\x4r_run'
export IGDEXT_TRACE=1 IGDEXT_SKIP_SELFTEST=1
export VKD3D_DEBUG=${VKD3D_DEBUG:-warn} DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-info}
[ -n "$X4R_OVR" ] && export VKD3D_SHADER_OVERRIDE=$R/../x4r/ovr
export WINEDEBUG=${WINEDEBUG:--all}
cd $R
timeout ${PROBE_SECONDS:-25} wine ./${EXE:-basic_sample_super_resolution_dx12.exe} "$@"
echo "exit: $?"
