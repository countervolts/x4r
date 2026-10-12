#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# refuse to start heavy work when the machine is short on memory (two hard freezes so far)
AV=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo); [ "$AV" -lt 8000 ] && { echo "only ${AV} MB available, refusing to start"; exit 9; }
# vkstats.sh <id...>: offline RADV statistics of a kernel compiled the way the real pipeline does it: HLSL -> DXIL (dxc) -> dxil-spirv (vkd3d's translator, with int8-dot/16-bit caps) -> RADV.
# Set X4R_DISPATCH=1 to use the old dispatcher form. Prints: VGPRs spills instructions latency
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; S="$X4R_SCRATCH"
export LD_LIBRARY_PATH=$R/deps/dl-dxc/lib
for i in "$@"; do
  hash=$(awk -v i=$i '$1==i{print $2}' $R/kernels/map.txt); rs=$(awk -v h=$hash '$1==h{print $2}' $R/kernels/kernel_rootsig.txt)
  opt=$R/kernels/spv/dyn_$i.opt
  lx=$(grep -o 'LWS_SIZE_X=[0-9]*' $opt | cut -d= -f2); ly=$(grep -o 'LWS_SIZE_Y=[0-9]*' $opt | cut -d= -f2); lz=$(grep -o 'LWS_SIZE_Z=[0-9]*' $opt | cut -d= -f2)
  $R/x4r/build/x4rwave $R/kernels/ll/dyn_$i.x.ll $R/prefix/drive_c/x4r/rs/$rs.txt $lx $ly $lz $S/vs_$i.hlsl || continue
  $R/x4r/tools/safe.sh $R/deps/dl-dxc/bin/dxc -T cs_6_6 -E main -Vd -enable-16bit-types ${DXC_OPT:--O0} -HV 2021 $S/vs_$i.hlsl -Fo $S/vs_$i.dxil 2>&1 | grep -i " error" | head -2
  $R/x4r/build/dxs/dxil-spirv $S/vs_$i.dxil --enable-shader-i8-dot --min-precision-native-16bit --subgroup-size 32 32 --output $S/vs_$i.spv > /dev/null 2>&1 || { echo "dyn_$i: dxil-spirv failed"; continue; }
  echo -n "dyn_$i: "; RADV_DEBUG=shaderstats $R/x4r/tools/safe.sh $R/x4r/build/vkpipe $S/vs_$i.spv 2>&1 | grep -E "^(VGPRs|Spilled VGPRs|Instructions|Latency|Inverse Throughput):" | sed 's/Spilled VGPRs/spills/; s/Inverse Throughput/invthr/; s/: /=/' | paste -sd' '
done
