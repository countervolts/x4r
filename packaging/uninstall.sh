#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Undo install.sh:  bash x4r/uninstall.sh [--prefix DIR]   (then delete igdext64.dll and the x4r folder from the game folder)
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
GAME=$(dirname -- "$HERE")
PFX=''
[[ ${1:-} != --prefix ]] || PFX=${2:?--prefix needs a directory}
if [[ -z $PFX ]]; then
  d=$GAME
  while [[ $d != / && $(basename -- "$(dirname -- "$d")") != common ]]; do d=$(dirname -- "$d"); done
  if [[ $d != / ]]; then
    inst=$(basename -- "$d"); lib=$(dirname -- "$(dirname -- "$d")")
    for m in "$lib"/appmanifest_*.acf; do
      [[ -f $m ]] && grep -q "\"installdir\"[[:space:]]*\"$inst\"" "$m" && { a=$(basename -- "$m" .acf); PFX=$lib/compatdata/${a#appmanifest_}/pfx; break; }
    done
  fi
fi
[[ -n $PFX ]] || { echo 'Pass --prefix <prefix dir>.' >&2; exit 1; }
SYS=$PFX/drive_c/windows/system32
if [[ -e $SYS/igdext64.dll.x4r-backup || -L $SYS/igdext64.dll.x4r-backup ]]; then
  rm -f -- "$SYS/igdext64.dll"; mv -- "$SYS/igdext64.dll.x4r-backup" "$SYS/igdext64.dll"; echo "Restored Proton's igdext64.dll"
else
  echo 'No backup found; Proton restores its own igdext64.dll when the prefix is next updated (or delete the prefix copy by hand).'
fi
if [[ -f $GAME/dxvk.conf ]] && grep -q '^# x4r begin' "$GAME/dxvk.conf"; then
  sed -i '/^# x4r begin/,/^# x4r end/d' "$GAME/dxvk.conf"; [[ -s $GAME/dxvk.conf ]] || rm -f "$GAME/dxvk.conf"; echo 'Removed the x4r block from dxvk.conf'
fi
