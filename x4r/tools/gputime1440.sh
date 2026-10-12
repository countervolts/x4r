#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# gputime1440.sh: like gputime.sh but maximises the sample window on the 2560x1440 monitor first
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; cd $R; rm -f prefix/drive_c/igdext_trace.log
EXE=${EXE:-sample1440.exe} PROBE_SECONDS=${SECS:-60} ./run-probe.sh > /dev/null 2>&1 &
sleep ${MAXAT:-14}
w=$(xdotool search --name "XeSS-SR" 2>/dev/null | head -1)
[ -n "$w" ] && { xdotool windowmove $w 0 0; xdotool windowsize $w 2560 1440; sleep 1; xdotool windowactivate $w 2>/dev/null; xdotool key --window $w super+Up 2>/dev/null; }
sleep 3; spectacle -b -n -f -o $R/shots/size1440.png >/dev/null 2>&1
wait
echo "title-based size check:"; identify $R/shots/size1440.png | cut -c1-80
python3 - <<'PY'
import re, os
L=open(os.environ['X4R_ROOT'] + '/prefix/drive_c/igdext_trace.log').read().split('\n')
idx=[i for i,l in enumerate(L) if 'GPU xess time' in l]
if not idx: raise SystemExit("no timing data")
names={}
for l in open(os.environ['X4R_ROOT'] + '/kernels/map.txt'):
    a,b=l.split(); names[b]=int(a)
dispatches={}
for l in L:
    m=re.search(r'x4r: dispatch ([0-9a-f]{16}) (\d+) (\d+) (\d+)',l)
    if m: dispatches.setdefault(names.get(m.group(1)),set()).add(tuple(map(int,m.groups()[1:])))
# These grids are confirmed against the fixed 2560x1440 RGBA16F capture
# harness (1280x720 Performance input), independent of desktop window size.
for kernel,grid in ((29,(80,360,1)),(2,(160,368,1))):
    if dispatches.get(kernel)!={grid}:
        raise SystemExit(f"1440p validation failed: dyn_{kernel} grids {dispatches.get(kernel)}; expected {grid}")
print("Validated XeSS output: 2560x1440; Performance input: 1280x720")
i=idx[-1]
print("XeSS GPU time per frame:", L[i].split('last 120): ')[1])
rows=[]
for l in L[i+1:i+40]:
    m=re.search(r'kernel ([0-9a-f]{16}): ([0-9.]+) ms/frame \((\d+) dispatches',l)
    if m: rows.append((float(m.group(2)),names[m.group(1)],int(m.group(3))))
for ms,k,n in sorted(rows,reverse=True)[:8]: print(f"  dyn_{k:<2} {ms:7.3f} ms  ({n}x)")
# dispatch sizes of the last frame (to infer resolution)
d=[l for l in L if 'x4r: dispatch' in l][-13:]
for l in d[:4]: print(l.split('] ')[1])
PY
