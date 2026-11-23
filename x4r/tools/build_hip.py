#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""build_hip.py: every translated kernel -> AMDGPU code object for the HIP backend (native Windows), named as the shim looks for it:
<out>/<kernel hash>.hsaco. x4rwave (lifted .x.ll + root signature -> wave HLSL) -> hlsl2hip.py -> clang (AMDGPU).
usage: build_hip.py --gpu gfx1101 --out <dir> [--llvm <clang dir>] [--wmma] [--ids 2,7,29] [--jobs N] [--stats]
env: X4R_PRIVATE (default <repo>/private: kernels/, runtime/rs/), X4R_BUILD (x4r/build-win: x4rwave.exe)
--wmma: x4rwave's WMMA flavour (X4R_WMMA=1) for the kernels that have one (RDNA3 / RDNA4 only)
--stats: print each kernel's VGPRs, scratch and occupancy (clang's resource-usage remarks)"""
import argparse, concurrent.futures as cf, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import hlsl2hip  # noqa: E402


def one(i, kh, a, priv, build, work):
    rsmap = one.rsmap
    rs = os.path.join(priv, 'runtime', 'rs', rsmap[kh] + '.txt')
    opt = open(os.path.join(priv, 'kernels', 'spv', f'dyn_{i}.opt')).read()
    lws = [re.search(f'LWS_SIZE_{c}=(\\d+)', opt).group(1) for c in 'XYZ']
    ll = os.path.join(priv, 'kernels', 'll', f'dyn_{i}.x.ll')
    hl, hip = os.path.join(work, f'dyn_{i}.hlsl'), os.path.join(work, f'dyn_{i}.hip')
    env = dict(os.environ)
    if a.wmma: env['X4R_WMMA'] = '1'
    else: env.pop('X4R_WMMA', None)
    r = subprocess.run([os.path.join(build, 'x4rwave.exe'), ll, rs, *lws, hl], env=env, capture_output=True, text=True)
    if r.returncode: return i, kh, 'x4rwave failed: ' + (r.stderr or r.stdout)[-300:]
    open(hip, 'w', newline='\n').write(hlsl2hip.convert(open(hl).read()))
    co = os.path.join(a.out, kh + '.hsaco')
    extra = ['-Rpass-analysis=kernel-resource-usage'] if a.stats else []
    try:
        r = hlsl2hip.build(hip, a.gpu, co, a.llvm, extra)
    except subprocess.CalledProcessError as e:
        return i, kh, 'clang failed: ' + str(e)
    info = ''
    if a.stats and r is not None:
        txt = r.stderr
        g = lambda k: (re.search(k + r': (\d+)', txt) or [None, '?'])[1]
        info = f'vgpr {g("VGPRs")} sgpr {g("SGPRs")} scratch {g("ScratchSize \\[bytes/lane\\]")} occupancy {g("Occupancy \\[waves/SIMD\\]")} lds {g("LDS Size \\[bytes/block\\]")}'
    return i, kh, 'ok ' + info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gpu', required=True); ap.add_argument('--out', required=True); ap.add_argument('--llvm')
    ap.add_argument('--wmma', action='store_true'); ap.add_argument('--ids'); ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--stats', action='store_true')
    a = ap.parse_args()
    priv = os.environ.get('X4R_PRIVATE', os.path.join(REPO, 'private'))
    build = os.environ.get('X4R_BUILD', os.path.join(REPO, 'x4r', 'build-win'))
    kmap = [l.split()[:2] for l in open(os.path.join(priv, 'kernels', 'map.txt')) if l.strip()]
    one.rsmap = dict(l.split()[:2] for l in open(os.path.join(priv, 'kernels', 'kernel_rootsig.txt')) if l.strip())
    if a.ids: want = set(a.ids.split(',')); kmap = [k for k in kmap if k[0] in want]
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix='x4rhip')
    bad = 0
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for i, kh, res in ex.map(lambda k: one(k[0], k[1], a, priv, build, work), kmap):
            if not res.startswith('ok'): bad += 1
            if a.stats or not res.startswith('ok'): print(f'dyn_{i} {kh} {res}', flush=True)
    open(os.path.join(a.out, 'flavour.txt'), 'w').write('wmma\n' if a.wmma else 'dot4\n')   # read by the shim (status.txt)
    print(f'{len(kmap) - bad}/{len(kmap)} code objects in {a.out}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
