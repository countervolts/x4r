#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tracecheck_hip.py: tools/tracecheck.sh for the HIP translation (native Windows): per-instruction trace of one workgroup of a
kernel, x4rwave's HLSL in trace mode -> tools/hlsl2hip.py -> hiprun, against the CPU oracle x4rsim, in windows of instructions;
same comparison and tolerances as tracecheck.sh.
usage: tracecheck_hip.py <dyn id> [window] [--gpu gfx1101]
env: X4R_PRIVATE (default <repo>/private: kernels/, runtime/rs/), X4R_BUILD (x4r/build-win: x4rwave.exe, x4rsim.exe, hiprun.exe),
     X4R_LLVM (clang with AMDGPU), X4R_SCRATCH (work dir)"""
import math, os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PRIV = os.environ.get('X4R_PRIVATE', os.path.join(REPO, 'private'))
BUILD = os.environ.get('X4R_BUILD', os.path.join(REPO, 'x4r', 'build-win'))
sys.path.insert(0, HERE)
import hlsl2hip  # noqa: E402


def f32(x): return struct.unpack('<f', struct.pack('<I', x))[0]


def close(x, y, tol=1e-30):
    if x == y: return True
    fx, fy = f32(x), f32(y)
    if math.isnan(fx) and math.isnan(fy): return True
    if math.isinf(fx) or math.isinf(fy): return False
    return abs(fx - fy) <= 1e-4 * max(abs(fx), abs(fy)) + tol


def compare(ref, gpu, trace, threads):
    a, b = open(ref, 'rb').read(), open(gpu, 'rb').read()
    lines = open(trace).read().split('\n'); tot = int(lines[0])
    ent = [l.split(' ', 3) for l in lines[1:] if l]
    for t in range(threads):
        for off, w, k, txt in [(int(e[0]), int(e[1]), e[2], e[3]) for e in ent]:
            base = (t * tot + off) * 4
            ra, rb = a[base:base + 4 * w], b[base:base + 4 * w]
            if ra == rb: continue
            wa, wb = struct.unpack('<%dI' % w, ra), struct.unpack('<%dI' % w, rb)
            isf = k[1] == 'F'
            tol = 2e-7 if re.search(r'native_(sin|cos|exp2|log2|powr|sqrt|recip)', txt) else 1e-30
            idx = [j for j in range(w) if wa[j] != wb[j] and not (isf and close(wa[j], wb[j], tol))]
            if idx:
                print(f'thread {t} FIRST MISMATCH ({len(idx)}/{w} words) at word {idx[0]}: oracle {wa[idx[0]]:08x} gpu {wb[idx[0]]:08x}\n  {txt[:300]}')
                return False
    return True


def main():
    args = sys.argv[1:]
    gpu = 'gfx1101'
    if '--gpu' in args:
        i = args.index('--gpu'); gpu = args[i + 1]; del args[i:i + 2]
    i, win = args[0], int(args[1]) if len(args) > 1 else 250
    kmap = dict(l.split()[:2] for l in open(os.path.join(PRIV, 'kernels', 'map.txt')) if l.strip())
    rsmap = dict(l.split()[:2] for l in open(os.path.join(PRIV, 'kernels', 'kernel_rootsig.txt')) if l.strip())
    rs = os.path.join(PRIV, 'runtime', 'rs', rsmap[kmap[i]] + '.txt')
    opt = open(os.path.join(PRIV, 'kernels', 'spv', f'dyn_{i}.opt')).read()
    lws = [int(re.search(f'LWS_SIZE_{c}=(\\d+)', opt).group(1)) for c in 'XYZ']
    ll = os.path.join(PRIV, 'kernels', 'll', f'dyn_{i}.x.ll')
    n = sum(1 for l in open(ll) if re.match(r'  %[^ ]+ = ', l))
    s = os.environ.get('X4R_SCRATCH') or tempfile.mkdtemp(prefix='x4rtc')
    os.makedirs(s, exist_ok=True)
    exe = lambda t: os.path.join(BUILD, t + '.exe')
    hl = os.path.join(s, f'wav_{i}.hlsl')
    lo = 0
    while lo < n:
        hi = lo + win
        subprocess.run([exe('x4rwave'), ll, rs, *map(str, lws), hl, 'trace', str(lo), str(hi)], check=True)
        ref = os.path.join(s, f'ref_{i}.bin.dbg')
        r = subprocess.run([exe('x4rsim'), ll, rs, *map(str, lws), hl + '.trace', ref, str(lo), str(hi), hl + '.layout'], timeout=600)
        if r.returncode:
            sys.exit(f'dyn_{i}: oracle failed; GPU run skipped')
        hip, co = os.path.join(s, f'wav_{i}.hip'), os.path.join(s, f'wav_{i}.hsaco')
        open(hip, 'w', newline='\n').write(hlsl2hip.convert(open(hl).read()))
        hlsl2hip.build(hip, gpu, co, os.environ.get('X4R_LLVM'))
        out = os.path.join(s, f'wav_{i}.bin')
        subprocess.run([exe('hiprun'), co, *map(str, lws), '1', '1', '1', out, '12345', hl + '.layout'], check=True, timeout=120)
        if not compare(ref, out + '.dbg', hl + '.trace', lws[0] * lws[1] * lws[2]):
            sys.exit(f'dyn_{i}: mismatch in window {lo}-{hi}')
        print(f'dyn_{i}: window {lo}-{hi} ok', flush=True)
        lo = hi
    print(f'dyn_{i}: no mismatch')


if __name__ == '__main__':
    main()
