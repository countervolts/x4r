#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# refuse to start heavy work when the machine is short on memory (two hard freezes so far)
AV=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo); [ "$AV" -lt 8000 ] && { echo "only ${AV} MB available, refusing to start"; exit 9; }
# build-kernel.sh <dyn id> : CM kernel (kernels/ll/dyn_N.opt.ll) -> HLSL -> DXIL under prefix/drive_c/x4r/dxil/<hash>.dxil
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
i=$1
hash=$(awk -v i=$i '$1==i{print $2}' $R/kernels/map.txt)
rs=$(awk -v h=$hash '$1==h{print $2}' $R/kernels/kernel_rootsig.txt)
opt=$R/kernels/spv/dyn_$i.opt
lx=$(grep -o 'LWS_SIZE_X=[0-9]*' $opt | cut -d= -f2); ly=$(grep -o 'LWS_SIZE_Y=[0-9]*' $opt | cut -d= -f2); lz=$(grep -o 'LWS_SIZE_Z=[0-9]*' $opt | cut -d= -f2)
DXDIR=${X4R_DXIL_DIR:-$R/prefix/drive_c/x4r/dxil}
mkdir -p "$R/x4r/out" "$DXDIR"
FWD=0; grep -q "define spir_kernel void @input_processing" $R/kernels/ll/dyn_$i.x.ll && FWD=3   # reading through region chains pays off only there (it spills everywhere else)
X4R_FWD=${X4R_FWD:-$FWD} "${X4R_TRANSLATOR:-$R/x4r/build/x4rwave}" $R/kernels/ll/dyn_$i.x.ll $R/prefix/drive_c/x4r/rs/$rs.txt $lx $ly $lz $R/x4r/out/$hash.hlsl || exit 2
LD_LIBRARY_PATH=$R/deps/dl-dxc/lib $R/x4r/tools/safe.sh $R/deps/dl-dxc/bin/dxc -T cs_6_6 -E main -Vd ${DXC_OPT:--O0} -HV 2021 -enable-16bit-types $R/x4r/out/$hash.hlsl -Fo "$DXDIR/$hash.dxil" || exit 3
echo "dyn_$i $hash ok"
