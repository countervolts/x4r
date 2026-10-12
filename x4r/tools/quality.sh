#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Build/generate/capture sequentially; GPU readbacks never depend on window size.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R"
mkdir -p quality
exec 9>quality/.lock
flock -n 9 || { echo 'Another quality job is active' >&2; exit 1; }
check_memory() {
    free -m
    local available
    available=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
    (( available >= 8000 )) || { echo "Only $available MB available" >&2; exit 9; }
}
case ${1:-} in
  build)
    check_memory
    x4r/tools/safe.sh x86_64-w64-mingw32-g++ -std=c++17 -O1 -static \
      -Ivendor/xess-sdk/inc x4r/src/quality.cpp vendor/xess-sdk/lib/libxess.lib \
      -ld3d12 -ldxgi -ldxguid -o x4r/build/quality.exe
    ;;
  scene)
    check_memory
    x4r/tools/safe.sh python3 x4r/tools/quality-scene.py
    ;;
  dp4a|dot4|wmma)
    mode=$1
    dest="$R/quality/$mode"
    mkdir -p "$dest"
    rm -f "$dest"/frame-{031,063,127}.rgba16f "$dest/provenance.json"
    # Use isolated directories so DXVK does not pick up x4r_run/dxvk.conf for DP4a.
    cp x4r/build/quality.exe "$dest/"
    for dll in d3d12.dll d3d12core.dll dxgi.dll; do
      cp "prefix/drive_c/$dll" "$dest/" 2>/dev/null || cp "prefix/drive_c/windows/system32/$dll" "$dest/"
    done
    cp prefix/drive_c/x4r_run/libxess.dll "$dest/"
    if [[ $mode != dp4a ]]; then
      cp prefix/drive_c/x4r_run/igdext64.dll "$dest/"
      cp prefix/drive_c/x4r_run/dxvk.conf "$dest/"
    else
      printf 'dxgi.hideAmdGpu = False\n' > "$dest/dxvk.conf"
    fi
    check_memory
    export WINEPREFIX="$R/prefix" WINEDLLOVERRIDES='d3d12,d3d12core,dxgi=n;mscoree,mshtml=' WINEDEBUG=-all
    export DXVK_CONFIG_FILE="$dest/dxvk.conf" DXVK_LOG_LEVEL=info VKD3D_DEBUG=warn
    unset VKD3D_SHADER_OVERRIDE INTC_ALT_DRIVER_EXTENSIONS_PATH X4R_OVR
    if [[ $mode != dp4a ]]; then
      export INTC_ALT_DRIVER_EXTENSIONS_PATH="Z:${dest//\//\\}"
      export IGDEXT_TRACE=1 IGDEXT_SKIP_SELFTEST=1
    fi
    [[ $mode != wmma ]] || export VKD3D_SHADER_OVERRIDE="${X4R_QUALITY_OVERRIDE:-$R/prefix/drive_c/x4r/ovr}"
    trace_offset=$(stat -c %s "$R/prefix/drive_c/igdext_trace.log" 2>/dev/null || echo 0)
    cd "$dest"
    "$R/x4r/tools/safe.sh" timeout 180 wine ./quality.exe \
      "Z:${R//\//\\}\\quality\\scene" "Z:${dest//\//\\}" >run.log 2>&1
    if [[ $mode != dp4a ]]; then
      tail -c +$((trace_offset+1)) "$R/prefix/drive_c/igdext_trace.log" > "$dest/igdext_trace.log"
    fi
    python3 - "$R" "$mode" <<'PY'
import hashlib,json,os,sys
import re
from pathlib import Path
r=Path(sys.argv[1]);mode=sys.argv[2];d=r/'quality'/mode
paths=[d/'quality.exe',d/'libxess.dll',r/'x4r/src/quality.cpp',r/'quality/scene/manifest.json']
if mode!='dp4a':
    paths += [d/'igdext64.dll',r/'prefix/drive_c/x4r/splitz.txt']
    paths += sorted((r/'prefix/drive_c/x4r/dxil').glob('*.dxil'))
if mode=='wmma': paths += sorted(Path(os.environ['VKD3D_SHADER_OVERRIDE']).glob('*.spv'))
paths += sorted(d.glob('frame-*.rgba16f'))
log=(d/'run.log').read_text()
expected_vendor='1002' if mode=='dp4a' else '8086'
if f'adapter vendor={expected_vendor}' not in log: raise RuntimeError('Wrong adapter vendor for requested backend')
loaded=set(re.findall(r'Overriding shader hash ([0-9a-f]{16})',log))
if mode=='wmma':
    kernels={int(i):h for i,h in (line.split() for line in (r/'kernels/map.txt').read_text().splitlines())}
    expected=set()
    for i in (12,7,25,27,28,26,29,13,18,23):
        data=(r/f'prefix/drive_c/x4r/dxil/{kernels[i]}.dxil').read_bytes()
        h=0xcbf29ce484222325
        for b in data: h=((h*0x100000001b3)&(2**64-1))^b
        expected.add(f'{h:016x}')
    data=(r/f'prefix/drive_c/x4r/dxil/{kernels[2]}.dxil').read_bytes()
    h=0xcbf29ce484222325
    for b in data: h=((h*0x100000001b3)&(2**64-1))^b
    if (Path(os.environ['VKD3D_SHADER_OVERRIDE'])/f'{h:016x}.spv').exists(): expected.add(f'{h:016x}')
    if not expected<=loaded: raise RuntimeError(f'Missing overrides: {expected-loaded}')
elif loaded: raise RuntimeError('Unexpected shader override in baseline')
if mode!='dp4a':
    trace=(d/'igdext_trace.log').read_text()
    if 'DUMMY' in trace: raise RuntimeError('Dummy Intel-extension pipeline used')
    if '-> x4r translated kernel' not in trace: raise RuntimeError('No translated Intel kernels created')
if len(list(d.glob('frame-*.rgba16f')))!=3: raise RuntimeError('Missing capture')
sha={str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
(d/'provenance.json').write_text(json.dumps(dict(mode=mode,sha256=sha,legacy_scale=True,
    frames=128,jitter_cycle=32,loaded_overrides=sorted(loaded),environment={k:os.environ.get(k) for k in
    ['DXVK_CONFIG_FILE','INTC_ALT_DRIVER_EXTENSIONS_PATH','VKD3D_SHADER_OVERRIDE']}),indent=2)+'\n')
PY
    tail -8 run.log
    ;;
  compare)
    check_memory
    x4r/tools/safe.sh python3 x4r/tools/quality-compare.py
    ;;
  *) echo 'usage: quality.sh build|scene|dp4a|dot4|wmma|compare' >&2; exit 2 ;;
esac
