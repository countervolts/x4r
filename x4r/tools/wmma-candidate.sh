#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Build validated SPIR-V into an isolated override directory, without installing.
# usage: wmma-candidate.sh <directory under project root> <dyn IDs...>
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R"
dest=$(realpath -m "$1"); shift
[[ $dest == "$(realpath -m "$R/quality")/"* ]] || { echo 'Candidate directory must be under quality/' >&2; exit 2; }
mkdir -p "$dest/artifacts"
free -m
export LD_LIBRARY_PATH="$R/deps/dl-dxc/lib"
for i in "$@"; do
  h=$(awk -v i="$i" '$1==i {print $2}' kernels/map.txt)
  rs=$(awk -v h="$h" '$1==h {print $2}' kernels/kernel_rootsig.txt)
  dims=()
  for a in X Y Z; do dims+=("$(sed -n "s/.*LWS_SIZE_$a=\([0-9]*\).*/\1/p" "kernels/spv/dyn_$i.opt")"); done
  out="$dest/artifacts/dyn_$i"
  X4R_SPLITZ=1 X4R_WMMA=1 "${X4R_TRANSLATOR:-$R/x4r/build/x4rwave}" \
    "kernels/ll/dyn_$i.x.ll" "prefix/drive_c/x4r/rs/$rs.txt" "${dims[@]}" "$out.flat.hlsl"
  python3 x4r/tools/bindless.py "$out.flat.hlsl" "prefix/drive_c/x4r/rs/$rs.txt" "$out.hlsl"
  x4r/tools/safe.sh deps/dl-dxc/bin/dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 \
    -fspv-use-vulkan-memory-model -I "$R/deps/dl-dxc/include/hlsl" -HV 2021 -enable-16bit-types \
    -O0 "$out.hlsl" -Fo "$out.spv" > "$out.dxc.log" 2>&1 || { head -20 "$out.dxc.log"; exit 3; }
  spirv-val --target-env vulkan1.3 "$out.spv"
  shader=$(python3 - "prefix/drive_c/x4r/dxil/$h.dxil" <<'PY'
import sys
h=0xcbf29ce484222325
for b in open(sys.argv[1],'rb').read(): h=((h*0x100000001b3)&(2**64-1))^b
print(f'{h:016x}')
PY
)
  cp "$out.spv" "$dest/$shader.spv"
  echo "dyn_$i -> $shader.spv"
done
