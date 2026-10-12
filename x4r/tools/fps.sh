#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# fps.sh: run the XeSS sample ~22 s and print the average fps / frame time from its window title
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; cd $R
PROBE_SECONDS=22 ./run-probe.sh > /dev/null 2>&1 &
sleep 15; tot=0; n=0
for k in 1 2 3 4 5 6; do
  w=$(xdotool search --name "XeSS-SR" 2>/dev/null | head -1)
  [ -n "$w" ] && { f=$(xdotool getwindowname $w | grep -o '[0-9]* fps' | grep -o '[0-9]*'); [ -n "$f" ] && { tot=$((tot+f)); n=$((n+1)); }; }
  sleep 1
done
wait
[ $n -gt 0 ] && python3 -c "f=$tot/$n; print('%.0f fps = %.2f ms/frame'%(f,1000/f))" || echo "no fps reading"
