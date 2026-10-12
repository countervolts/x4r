#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# crosscheck.sh <dyn id> [gx gy gz]: run the scalar reference translation and the wave translation on identical random inputs and compare outputs
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; S="$X4R_SCRATCH"
i=$1; gx=${2:-3}; gy=${3:-2}; gz=${4:-1}
hash=$(awk -v i=$i '$1==i{print $2}' $R/kernels/map.txt); rs=$(awk -v h=$hash '$1==h{print $2}' $R/kernels/kernel_rootsig.txt)
opt=$R/kernels/spv/dyn_$i.opt
lx=$(grep -o 'LWS_SIZE_X=[0-9]*' $opt | cut -d= -f2); ly=$(grep -o 'LWS_SIZE_Y=[0-9]*' $opt | cut -d= -f2); lz=$(grep -o 'LWS_SIZE_Z=[0-9]*' $opt | cut -d= -f2)
export LD_LIBRARY_PATH=$R/deps/dl-dxc/lib
D="$R/x4r/tools/safe.sh $R/deps/dl-dxc/bin/dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 -HV 2021 -enable-16bit-types -fvk-t-shift 0 0 -fvk-u-shift 16 0 -fvk-b-shift 32 0"
$R/x4r/build/x4rtrans $R/kernels/ll/dyn_$i.opt.ll $R/prefix/drive_c/x4r/rs/$rs.txt $lx $ly $lz $S/ref_$i.hlsl || exit 2
$R/x4r/build/x4rwave $R/kernels/ll/dyn_$i.x.ll $R/prefix/drive_c/x4r/rs/$rs.txt $lx $ly $lz $S/wav_$i.hlsl || exit 2
grep -q "Texture2D" $S/wav_$i.hlsl && { echo "dyn_$i: has images, skipped"; exit 0; }
$D -O3 $S/ref_$i.hlsl -Fo $S/ref_$i.spv 2>&1 | grep -i " error" | head -3
$D -O3 $S/wav_$i.hlsl -Fo $S/wav_$i.spv 2>&1 | grep -i " error" | head -3
timeout 300 $R/x4r/tools/safe.sh $R/x4r/build/vkrun $S/ref_$i.spv $gx $gy $gz $S/ref_$i.bin || { echo "dyn_$i: ref run failed"; exit 3; }
timeout 300 $R/x4r/tools/safe.sh $R/x4r/build/vkrun $S/wav_$i.spv $gx $gy $gz $S/wav_$i.bin || { echo "dyn_$i: wave run failed"; exit 3; }
python3 - $S/ref_$i.bin $S/wav_$i.bin $i <<'PY'
import sys, struct
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read(); i=sys.argv[3]
BUF=4<<20
nz=sum(1 for k in range(0,len(a),4) if a[k:k+4]!=b'\0\0\0\0')
diff=[k//4 for k in range(0,len(a),4) if a[k:k+4]!=b[k:k+4]]
print(f"dyn_{i}: ref nonzero words {nz}, differing words {len(diff)}")
for d in diff[:6]:
    print("  buf u%d word %d: ref %08x wave %08x" % (d*4//BUF, (d*4%BUF)//4, struct.unpack('<I',a[d*4:d*4+4])[0], struct.unpack('<I',b[d*4:d*4+4])[0]))
PY
