#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# build-wmma.sh <dyn id>... : WMMA flavour of a kernel as direct SPIR-V, installed as a vkd3d-proton shader override
# (VKD3D_SHADER_OVERRIDE=prefix/drive_c/x4r/ovr, file name = FNV-1 64 of the DXIL that the shim hands to vkd3d)
set -euo pipefail
AV=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo); [ "$AV" -lt 8000 ] && { echo "only ${AV} MB available, refusing to start"; exit 9; }
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; export LD_LIBRARY_PATH=$R/deps/dl-dxc/lib
source "$R/x4r/tools/fast-profile.sh"
S="$X4R_SCRATCH"; O=${X4R_OVR_DIR:-$R/prefix/drive_c/x4r/ovr}; mkdir -p $O
for i in "$@"; do
  [[ ${X4R_PROFILE:-} != fast ]] || x4r_fast_profile "$i" spirv || exit 2
  hash=$(awk -v i=$i '$1==i{print $2}' $R/kernels/map.txt); rs=$(awk -v h=$hash '$1==h{print $2}' $R/kernels/kernel_rootsig.txt)
  opt=$R/kernels/spv/dyn_$i.opt
  lx=$(grep -o 'LWS_SIZE_X=[0-9]*' $opt|cut -d= -f2); ly=$(grep -o 'LWS_SIZE_Y=[0-9]*' $opt|cut -d= -f2); lz=$(grep -o 'LWS_SIZE_Z=[0-9]*' $opt|cut -d= -f2)
  X4R_SPLITZ=1 X4R_WMMA=1 "${X4R_TRANSLATOR:-$R/x4r/build/x4rwave}" $R/kernels/ll/dyn_$i.x.ll $R/prefix/drive_c/x4r/rs/$rs.txt $lx $ly $lz $S/bl_$i.flat.hlsl || exit 2
  touch "$R/prefix/drive_c/x4r/splitz.txt"
  sed -i "/^$hash /d" $R/prefix/drive_c/x4r/splitz.txt
  if grep -q "gidIn.z / " $S/bl_$i.flat.hlsl; then echo "$hash $lz" >> $R/prefix/drive_c/x4r/splitz.txt; fi   # shim scales the dispatch
  python3 $R/x4r/tools/bindless.py $S/bl_$i.flat.hlsl $R/prefix/drive_c/x4r/rs/$rs.txt $S/bl_$i.hlsl || { echo "dyn_$i: skipped"; continue; }
  $R/x4r/tools/safe.sh $R/deps/dl-dxc/bin/dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 -fspv-use-vulkan-memory-model -I $R/deps/dl-dxc/include/hlsl -HV 2021 -enable-16bit-types -O0 $S/bl_$i.hlsl -Fo $S/bl_$i.spv > "$S/bl_$i.dxc.log" 2>&1 || { head -20 "$S/bl_$i.dxc.log"; exit 3; }
  spirv-val --target-env vulkan1.3 $S/bl_$i.spv || exit 3
  h=$(python3 -c "
import sys
d=open('$R/prefix/drive_c/x4r/dxil/$hash.dxil','rb').read(); h=0xcbf29ce484222325
for b in d: h=((h*0x100000001b3)&(2**64-1))^b
print('%016x'%h)")
  cp $S/bl_$i.spv $O/$h.spv; echo "dyn_$i ($hash) -> $h.spv"
done
