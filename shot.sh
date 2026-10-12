#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# shot.sh <name> [baseline]: run the XeSS sample and take a screenshot to shots/<name>.png. "baseline" hides the shim (DP4a path).
source "$(dirname -- "${BASH_SOURCE[0]}")/x4r/tools/common.sh"
cd "$X4R_ROOT"
R=prefix/drive_c/x4r_run
[ "$2" = baseline ] && mv $R/igdext64.dll $R/igdext64.dll.off
PROBE_SECONDS=${SECS:-40} ./run-probe.sh > shots/$1.log 2>&1 &
sleep ${DLG_AT:-10}
if [ "$2" = baseline ]; then for w in $(xdotool search --name "Important notice" 2>/dev/null); do xdotool key --window $w Return; done; fi
sleep ${SHOT_AT:-14}
spectacle -b -n -f -o shots/$1.png >/dev/null 2>&1
wait
[ "$2" = baseline ] && mv $R/igdext64.dll.off $R/igdext64.dll
echo shot-done
