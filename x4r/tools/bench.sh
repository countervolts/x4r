#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# refuse to start heavy work when the machine is short on memory (two hard freezes so far)
AV=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo); [ "$AV" -lt 8000 ] && { echo "only ${AV} MB available, refusing to start"; exit 9; }
# bench.sh <dyn id> <gx> <gy> <gz> : GPU time of one kernel at a given dispatch size on random data (the CPU oracle must have proven termination first: tracecheck.sh)
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; S="$X4R_SCRATCH"
i=$1; export LD_LIBRARY_PATH=$R/deps/dl-dxc/lib
hash=$(awk -v i=$i '$1==i{print $2}' $R/kernels/map.txt); rs=$(awk -v h=$hash '$1==h{print $2}' $R/kernels/kernel_rootsig.txt)
opt=$R/kernels/spv/dyn_$i.opt
lx=$(grep -o 'LWS_SIZE_X=[0-9]*' $opt | cut -d= -f2); ly=$(grep -o 'LWS_SIZE_Y=[0-9]*' $opt | cut -d= -f2); lz=$(grep -o 'LWS_SIZE_Z=[0-9]*' $opt | cut -d= -f2)
$R/x4r/build/x4rwave $R/kernels/ll/dyn_$i.x.ll $R/prefix/drive_c/x4r/rs/$rs.txt $lx $ly $lz $S/bn_$i.hlsl || exit 2
$R/x4r/tools/safe.sh $R/deps/dl-dxc/bin/dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 -fspv-use-vulkan-memory-model -I $R/deps/dl-dxc/include/hlsl -HV 2021 -enable-16bit-types ${DXC_OPT:--O0} -fvk-t-shift 0 0 -fvk-u-shift 16 0 -fvk-b-shift 32 0 -fvk-s-shift 52 0 $S/bn_$i.hlsl -Fo $S/bn_$i.spv 2>&1 | grep -i " error" | head -2
X4R_TIME=${REPS:-5} timeout 120 $R/x4r/tools/safe.sh $R/x4r/build/vkrun $S/bn_$i.spv $2 $3 $4 $S/bn_$i.bin 12345 $S/bn_$i.hlsl.layout
