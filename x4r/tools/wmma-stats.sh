#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Inspect native ISA for a flat-resource WMMA shader after oracle validation.
# This is a compiler diagnostic; final decisions use the real sample timings.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R"
i=$1; out=$(realpath -m "$2")
[[ $out == "$(realpath -m "$R/quality")/"* ]] || exit 2
mkdir -p "$(dirname "$out")"
free -m
h=$(awk -v i="$i" '$1==i {print $2}' kernels/map.txt)
rs=$(awk -v h="$h" '$1==h {print $2}' kernels/kernel_rootsig.txt)
dims=()
for a in X Y Z; do dims+=("$(sed -n "s/.*LWS_SIZE_$a=\([0-9]*\).*/\1/p" "kernels/spv/dyn_$i.opt")"); done
X4R_WMMA=1 "${X4R_TRANSLATOR:-$R/x4r/build/x4rwave}" "kernels/ll/dyn_$i.x.ll" "prefix/drive_c/x4r/rs/$rs.txt" "${dims[@]}" "$out.hlsl"
export LD_LIBRARY_PATH="$R/deps/dl-dxc/lib"
x4r/tools/safe.sh deps/dl-dxc/bin/dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 \
 -fspv-use-vulkan-memory-model -I "$R/deps/dl-dxc/include/hlsl" -HV 2021 -enable-16bit-types -O0 \
 -fvk-t-shift 0 0 -fvk-u-shift 16 0 -fvk-b-shift 32 0 -fvk-s-shift 52 0 \
 "$out.hlsl" -Fo "$out.spv" > "$out.dxc.log" 2>&1
spirv-val --target-env vulkan1.3 "$out.spv"
RADV_DEBUG=shaderstats,shaders x4r/tools/safe.sh x4r/build/vkrun "$out.spv" 1 1 1 "$out.bin" 12345 "$out.hlsl.layout" > "$out.isa" 2>&1
rg '^(VGPRs|Spilled VGPRs|Instructions|Latency|Inverse Throughput):' "$out.isa"
rm -f "$out.bin" "$out.bin.dbg"
