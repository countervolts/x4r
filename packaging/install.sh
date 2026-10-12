#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# One-time setup. Run from the game folder you extracted the zip into:  bash x4r/install.sh [--prefix DIR]
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
GAME=$(dirname -- "$HERE")
PFX=''
fail() { printf 'x4r install: %s\n' "$*" >&2; exit 1; }
while [[ $# -gt 0 ]]; do
  case $1 in
    --prefix) PFX=${2:?--prefix needs a directory}; shift 2 ;;
    -h|--help) echo 'Usage: bash x4r/install.sh [--prefix <Wine/Proton prefix>]'; echo 'Steam games are found automatically; give --prefix for other launchers (the folder holding drive_c).'; exit 0 ;;
    *) fail "unknown option $1" ;;
  esac
done
[[ -f $GAME/igdext64.dll && -d $HERE/dxil ]] || fail "extract the release zip into the game's .exe folder first (no igdext64.dll beside $HERE)"

if [[ -z $PFX ]]; then
  # <library>/steamapps/common/<installdir>/...  ->  <library>/steamapps/compatdata/<appid>/pfx
  d=$GAME; lib=''; inst=''
  while [[ $d != / ]]; do
    if [[ $(basename -- "$(dirname -- "$d")") == common && $(basename -- "$(dirname -- "$(dirname -- "$d")")") == steamapps ]]; then
      inst=$(basename -- "$d"); lib=$(dirname -- "$(dirname -- "$d")"); break
    fi
    d=$(dirname -- "$d")
  done
  [[ -n $lib ]] || fail 'not inside a Steam library (steamapps/common/<game>). Pass --prefix <prefix dir>.'
  appid=''
  for m in "$lib"/appmanifest_*.acf; do
    [[ -f $m ]] || continue
    if grep -q "\"installdir\"[[:space:]]*\"$inst\"" "$m"; then appid=$(basename -- "$m" .acf); appid=${appid#appmanifest_}; break; fi
  done
  [[ -n $appid ]] || fail "no Steam app manifest for '$inst'. Pass --prefix <prefix dir>."
  PFX=$lib/compatdata/$appid/pfx
  [[ -d $PFX ]] || fail "no Proton prefix yet at $PFX: launch the game once with Proton, close it, and run this again."
fi
SYS=$PFX/drive_c/windows/system32
[[ -d $SYS ]] || fail "$PFX is not a Wine prefix (no drive_c/windows/system32)"

# 1. the shim: XeSS loads igdext64.dll from the prefix's system32 (Proton fixes that path), so it replaces Proton's copy
if cmp -s "$GAME/igdext64.dll" "$SYS/igdext64.dll"; then
  echo 'igdext64.dll is already installed in the prefix.'
else
  if [[ -e $SYS/igdext64.dll || -L $SYS/igdext64.dll ]] && [[ ! -e $SYS/igdext64.dll.x4r-backup ]]; then
    cp -p --no-dereference -- "$SYS/igdext64.dll" "$SYS/igdext64.dll.x4r-backup"
    echo "Saved Proton's igdext64.dll as $SYS/igdext64.dll.x4r-backup"
  fi
  rm -f -- "$SYS/igdext64.dll"
  cp -- "$GAME/igdext64.dll" "$SYS/igdext64.dll"
  echo "Installed $SYS/igdext64.dll"
fi

# 2. make the AMD GPU look like an Intel one to the game (DXVK reads dxvk.conf from the game folder)
CONF=$GAME/dxvk.conf
if [[ -f $CONF ]] && grep -q '^# x4r begin' "$CONF"; then
  echo 'dxvk.conf already has the x4r block.'
else
  [[ ! -f $CONF ]] || cp -p -- "$CONF" "$CONF.x4r-backup.$(date +%s)"
  { [[ ! -s $CONF ]] || [[ $(tail -c1 "$CONF" | wc -l) -gt 0 ]] || echo; cat <<'CFG'
# x4r begin (removed by x4r/uninstall.sh)
dxgi.customVendorId = 8086
dxgi.customDeviceId = e20b
dxgi.hideAmdGpu = False
# x4r end
CFG
  } >> "$CONF"
  echo "Updated $CONF"
fi
printf 'installed %s into %s\n' "$(cat "$HERE/VERSION")" "$PFX" > "$HERE/installation-status.txt"
cat <<MSG

Done. Use GE-Proton 11 (or Proton with vkd3d-proton 3.x), select XeSS in the game's DX12 renderer, and launch.
Proton replaces igdext64.dll when it updates a prefix; run this script again if XeSS looks like plain DP4a afterwards.
No launch options are needed.
MSG
