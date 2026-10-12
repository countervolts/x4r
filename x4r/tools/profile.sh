#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# profile.sh: frame time with each network kernel replaced by a no-op (fps read from the sample's window title). Output: x4r/out/profile.txt
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; D=$R/prefix/drive_c/x4r/dxil; OFF=$R/prefix/drive_c/x4r/dxil_off; mkdir -p $OFF
run() {   # $1 label
  cd $R; PROBE_SECONDS=${SECS:-24} ./run-probe.sh > /dev/null 2>&1 &
  sleep 17; tot=0; n=0
  for k in 1 2 3 4; do
    for w in $(xdotool search --name "XeSS-SR" 2>/dev/null | head -1); do t=$(xdotool getwindowname $w); f=$(echo "$t" | grep -o '[0-9]* fps' | grep -o '[0-9]*'); [ -n "$f" ] && { tot=$((tot+f)); n=$((n+1)); }; done; sleep 1.5
  done
  wait; [ $n -gt 0 ] && echo "$1 $((tot/n))" >> $R/x4r/out/profile.txt || echo "$1 0" >> $R/x4r/out/profile.txt
}
: > $R/x4r/out/profile.txt
run baseline
for i in 2 7 12 13 18 23 24 25 26 27 28 29; do
  h=$(awk -v i=$i '$1==i{print $2}' $R/kernels/map.txt)
  mv $D/$h.dxil $OFF/; run dyn_$i; mv $OFF/$h.dxil $D/
done
echo profile-done >> $R/x4r/out/profile.txt
