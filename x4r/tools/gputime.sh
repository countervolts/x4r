#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# gputime.sh: run the sample ~22 s and print the real per-kernel GPU times measured by the shim (timestamps around each dispatch)
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; cd $R; rm -f prefix/drive_c/igdext_trace.log
PROBE_SECONDS=${SECS:-22} ./run-probe.sh > /dev/null 2>&1
python3 - <<'PY'
import re, os
L=open(os.environ['X4R_ROOT'] + '/prefix/drive_c/igdext_trace.log').read().split('\n')
idx=[i for i,l in enumerate(L) if 'GPU xess time' in l]
if not idx: print("no timing data"); raise SystemExit
i=idx[-1]; names={}
for l in open(os.environ['X4R_ROOT'] + '/kernels/map.txt'):
    a,b=l.split(); names[b]=int(a)
print("XeSS GPU time per frame:", L[i].split('last 120): ')[1])
rows=[]
for l in L[i+1:i+40]:
    m=re.search(r'kernel ([0-9a-f]{16}): ([0-9.]+) ms/frame \((\d+) dispatches',l)
    if m: rows.append((float(m.group(2)),names[m.group(1)],int(m.group(3))))
for ms,k,n in sorted(rows,reverse=True)[:8]: print(f"  dyn_{k:<2} {ms:7.3f} ms  ({n}x)")
PY
