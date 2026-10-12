#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Offline statistics for the opt-in move E-form experiment, through dxil-spirv.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R"
free -m
AV=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
((AV>=8000)) || exit 9
export LD_LIBRARY_PATH="$R/deps/dl-dxc/lib" X4R_FWD=3
h=$(awk '$1==2 {print $2}' kernels/map.txt)
rs=$(awk -v h="$h" '$1==h {print $2}' kernels/kernel_rootsig.txt)
mkdir -p quality/packing
for mode in baseline move; do
    export X4R_MOVE_EFORM=0
    [[ $mode != move ]] || export X4R_MOVE_EFORM=1
    out="$R/quality/packing/$mode"
    X4R_COST=1 x4r/build/x4rwave-experiment kernels/ll/dyn_2.x.ll "prefix/drive_c/x4r/rs/$rs.txt" 2 1 1 "$out.hlsl" 2> "$out.cost"
    x4r/tools/safe.sh deps/dl-dxc/bin/dxc -T cs_6_6 -E main -Vd -O0 -HV 2021 -enable-16bit-types "$out.hlsl" -Fo "$out.dxil"
    x4r/tools/safe.sh x4r/build/dxs/dxil-spirv "$out.dxil" --enable-shader-i8-dot --min-precision-native-16bit --subgroup-size 32 32 --output "$out.spv" > "$out.convert.log" 2>&1
    RADV_DEBUG=shaderstats x4r/tools/safe.sh x4r/build/vkpipe "$out.spv" > "$out.stats" 2>&1
    echo "$mode:"
    rg '^(VGPRs|Spilled VGPRs|Instructions|Latency|Inverse Throughput):' "$out.stats"
done
