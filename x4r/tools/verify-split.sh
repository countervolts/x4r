#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Validate generic operations with windowed CPU traces, then compare split WMMA
# outputs with an unsplit dot4 shader on the same deterministic resources.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
cd "$R"
i=$1
[[ $i == 13 || $i == 18 || $i == 23 ]] || exit 2
free -m
TX=${X4R_TRANSLATOR:-$R/x4r/build/x4rwave}
env -u X4R_WMMA -u X4R_SPLITZ bash x4r/tools/tracecheck.sh "$i" 200
h=$(awk -v i="$i" '$1==i {print $2}' kernels/map.txt)
rs=$(awk -v h="$h" '$1==h {print $2}' kernels/kernel_rootsig.txt)
dims=()
for a in X Y Z; do dims+=("$(sed -n "s/.*LWS_SIZE_$a=\([0-9]*\).*/\1/p" "kernels/spv/dyn_$i.opt")"); done
out="$S/splitcheck-$i"
export LD_LIBRARY_PATH="$R/deps/dl-dxc/lib"
for mode in reference candidate; do
  if [[ $mode == reference ]]; then
    env -u X4R_WMMA -u X4R_SPLITZ "$TX" "kernels/ll/dyn_$i.x.ll" "prefix/drive_c/x4r/rs/$rs.txt" "${dims[@]}" "$out-$mode.hlsl"
  else
    X4R_WMMA=1 X4R_SPLITZ=1 "$TX" "kernels/ll/dyn_$i.x.ll" "prefix/drive_c/x4r/rs/$rs.txt" "${dims[@]}" "$out-$mode.hlsl"
  fi
  x4r/tools/safe.sh deps/dl-dxc/bin/dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 -fspv-use-vulkan-memory-model \
    -I "$R/deps/dl-dxc/include/hlsl" -HV 2021 -enable-16bit-types -O0 \
    -fvk-t-shift 0 0 -fvk-u-shift 16 0 -fvk-b-shift 32 0 -fvk-s-shift 52 0 \
    "$out-$mode.hlsl" -Fo "$out-$mode.spv" > "$out-$mode.dxc.log" 2>&1
  spirv-val --target-env vulkan1.3 "$out-$mode.spv"
  z=1
  [[ $mode != candidate ]] || z=${dims[2]}
  x4r/tools/safe.sh timeout 120 x4r/build/vkrun "$out-$mode.spv" 1 1 "$z" "$out-$mode.bin" 12345 "$out-$mode.hlsl.layout"
done
cmp "$out-reference.bin" "$out-candidate.bin"
rm -f "$out-reference.bin" "$out-candidate.bin" "$out-reference.bin.dbg" "$out-candidate.bin.dbg"
echo "dyn_$i: split output byte-identical to oracle-checked dot4"
