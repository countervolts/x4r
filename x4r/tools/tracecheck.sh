#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# refuse to start heavy work when the machine is short on memory (two hard freezes so far)
AV=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo); [ "$AV" -lt 8000 ] && { echo "only ${AV} MB available, refusing to start"; exit 9; }
# tracecheck.sh <dyn id> [window]: per-instruction trace of the wave translation vs the CPU oracle (1 workgroup), in windows of
# instructions so the trace-mode shader stays small (a whole-kernel trace shader needs >12 GB in dxc).
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; S="$X4R_SCRATCH"
i=$1; WIN=${2:-250}
hash=$(awk -v i=$i '$1==i{print $2}' $R/kernels/map.txt); rs=$(awk -v h=$hash '$1==h{print $2}' $R/kernels/kernel_rootsig.txt)
opt=$R/kernels/spv/dyn_$i.opt
lx=$(grep -o 'LWS_SIZE_X=[0-9]*' $opt | cut -d= -f2); ly=$(grep -o 'LWS_SIZE_Y=[0-9]*' $opt | cut -d= -f2); lz=$(grep -o 'LWS_SIZE_Z=[0-9]*' $opt | cut -d= -f2)
export LD_LIBRARY_PATH=$R/deps/dl-dxc/lib
D="$R/x4r/tools/safe.sh $R/deps/dl-dxc/bin/dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 -fspv-use-vulkan-memory-model -I $R/deps/dl-dxc/include/hlsl -HV 2021 -enable-16bit-types -fvk-t-shift 0 0 -fvk-u-shift 16 0 -fvk-b-shift 32 0 -fvk-s-shift 52 0"
N=$(grep -cE '^  %[^ ]+ = ' $R/kernels/ll/dyn_$i.x.ll)   # upper bound on traced instruction count
lo=0; bad=0
while [ $lo -lt $N ]; do
  hi=$((lo+WIN))
  ${X4R_TRANSLATOR:-$R/x4r/build/x4rwave} $R/kernels/ll/dyn_$i.x.ll $R/prefix/drive_c/x4r/rs/$rs.txt $lx $ly $lz $S/wav_$i.hlsl trace $lo $hi || exit 2
  # prove on the CPU that the kernel terminates BEFORE the GPU runs it (a runaway GPU job can hang the whole machine)
  timeout 300 $R/x4r/tools/safe.sh $R/x4r/build/x4rsim $R/kernels/ll/dyn_$i.x.ll $R/prefix/drive_c/x4r/rs/$rs.txt $lx $ly $lz $S/wav_$i.hlsl.trace $S/ref_$i.bin.dbg $lo $hi $S/wav_$i.hlsl.layout || { echo "dyn_$i: oracle failed or did not terminate; GPU run skipped"; exit 4; }
  rm -f $S/wav_$i.spv
  $D ${DXC_OPT:--O0} $S/wav_$i.hlsl -Fo $S/wav_$i.spv > $S/wav_$i.dxc.log 2>&1 || { head -20 $S/wav_$i.dxc.log; exit 5; }
  [ -f $S/wav_$i.spv ] || { echo "dyn_$i: dxc failed (window $lo-$hi)"; exit 5; }
  timeout 120 $R/x4r/tools/safe.sh $R/x4r/build/vkrun $S/wav_$i.spv 1 1 1 $S/wav_$i.bin 12345 $S/wav_$i.hlsl.layout || { echo "wave run failed"; exit 3; }
  python3 - $S/ref_$i.bin.dbg $S/wav_$i.bin.dbg $S/wav_$i.hlsl.trace $((lx*ly*lz)) <<'PY' || bad=1
import sys, struct, re, math
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read(); T=int(sys.argv[4])
lines=open(sys.argv[3]).read().split('\n'); tot=int(lines[0]); ent=[l.split(' ',3) for l in lines[1:] if l]  # off W kind text
fl=re.compile(r'= (?:tail call |call )?(?:[a-z_]+ )*(?:<\d+ x float>|float\b)')
def f32(x): return struct.unpack('<f',struct.pack('<I',x))[0]
def close(x,y,tol=1e-30):
    if x==y: return True
    fx,fy=f32(x),f32(y)
    if math.isnan(fx) and math.isnan(fy): return True
    if math.isinf(fx) or math.isinf(fy): return False
    return abs(fx-fy) <= 1e-4*max(abs(fx),abs(fy)) + tol   # GPU: denormals flushed, fma contraction, native log2/exp2 ulp
for t in range(T):
  for off,w,k,txt in [(int(e[0]),int(e[1]),e[2],e[3]) for e in ent]:
    base=(t*tot+off)*4
    ra=a[base:base+4*w]; rb=b[base:base+4*w]
    if ra!=rb:
        wa=struct.unpack('<%dI'%w,ra); wb=struct.unpack('<%dI'%w,rb)
        isf=(k[1]=='F'); tol=2e-7 if re.search(r'native_(sin|cos|exp2|log2|powr|sqrt|recip)',txt) else 1e-30
        idx=[j for j in range(w) if wa[j]!=wb[j] and not (isf and close(wa[j],wb[j],tol))]
        if not idx: continue
        print(f"thread {t} FIRST MISMATCH ({len(idx)}/{w} words) at word {idx[0]}: oracle {wa[idx[0]]:08x} gpu {wb[idx[0]]:08x}\n  {txt[:300]}")
        sys.exit(1)
PY
  [ $bad = 1 ] && { echo "dyn_$i: mismatch in window $lo-$hi"; exit 1; }
  lo=$hi
done
echo "dyn_$i: no mismatch"
