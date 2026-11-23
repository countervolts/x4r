#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""hlsl2hip.py: x4rwave's HLSL of a kernel -> HIP device code (src/x4r_hip.h) -> AMDGPU code object, for x4r on native Windows
(AMD's HIP runtime). x4rwave's output is regular (resources at file scope with explicit registers, one main with SV_GroupID and
SV_GroupIndex, helper functions, constant tables, groupshared arrays), so this is a line-level rewrite:
  resources                 -> macros over the kernel argument (X4rArgs: slot address + size by vkrun's binding numbering)
  main(...)                 -> extern "C" kernel x4r_main(X4rArgs a) with the group id and flat thread index made up front
  helpers / tables / slm    -> __device__ functions, constant tables, __shared__ arrays
  float4(...) and friends   -> X4R_V_float4(...) (clang has no multi-argument vector constructor)
  uint(...), int(...)       -> x4r_uint / x4r_int (saturating from float, as D3D12 and CM)
  [branch] [loop] [unroll]  -> dropped
usage: hlsl2hip.py in.hlsl out.hip            (writes the .hip; --build GPU OUT.hsaco also compiles it: clang from --llvm or PATH)
       hlsl2hip.py in.hlsl out.hip --build gfx1101 out.hsaco [--llvm <dir with clang>]"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRELUDE = os.path.join(os.path.dirname(HERE), 'src', 'x4r_hip.h')
SHIFT = {'t': 0, 'u': 16, 'b': 32, 's': 52}


def convert(text):
    out, body, shared, threads, in_main = [], [], [], None, False
    for line in text.split('\n'):
        m = re.match(r'\[numthreads\((\d+),(\d+),(\d+)\)\]', line)
        if m:
            threads = tuple(int(x) for x in m.groups())
            continue
        if line.startswith('[WaveSize('):
            continue
        # the WMMA flavour's SPIR-V cooperative-matrix declarations: x4r_hip.h has HIP versions of them
        if line.startswith('#include "vk/') or line.startswith('typedef vk::') or line.startswith('[[vk::ext_extension'):
            continue
        m = re.match(r'(RW)?(Texture2D<float4>|ByteAddressBuffer) (\w+) : register\(([tu])(\d+)\);', line)
        if m:
            slot = SHIFT[m.group(4)] + int(m.group(5))
            if 'Texture' in m.group(2):
                out.append(f'#define {m.group(3)} {"X4rRwTex" if m.group(1) else "X4rTex"}{{a.slot[{slot}]}}')
            else:
                out.append(f'#define {m.group(3)} X4rBuf::make(a.slot[{slot}], a.size[{slot}])')
            continue
        m = re.match(r'SamplerState (\w+) : register\(s(\d+)\);', line)
        if m:
            out.append(f'#define {m.group(1)} X4rSmp{{a.slot[{SHIFT["s"] + int(m.group(2))}]}}')
            continue
        m = re.match(r'cbuffer \w+ : register\(b(\d+)\) \{ uint4 (\w+)\[\d+\]; \};', line)
        if m:
            out.append(f'#define {m.group(2)} ((__attribute__((address_space(4))) const uint4*)a.slot[{SHIFT["b"] + int(m.group(1))}])')
            continue
        m = re.match(r'groupshared (\w+) (\w+)\[(\d+)\];', line)
        if m:
            shared.append(f'__shared__ {m.group(1)} {m.group(2)}[{m.group(3)}];')
            continue
        if line.startswith('static const '):
            out.append('__constant__ ' + line[len('static '):])
            continue
        if re.match(r'void main\(uint3 gidIn : SV_GroupID, uint gidx : SV_GroupIndex\) \{', line):
            if not threads:
                raise SystemExit('hlsl2hip: no [numthreads] before main')
            n = threads[0] * threads[1] * threads[2]
            out += shared
            out.append(f'extern "C" __global__ __attribute__((amdgpu_flat_work_group_size({n}, {n}))) void x4r_main(X4rArgs a) {{')
            out.append('  const uint3 gidIn = uint3{__builtin_amdgcn_workgroup_id_x(), __builtin_amdgcn_workgroup_id_y(), __builtin_amdgcn_workgroup_id_z()};')
            out.append(f'  const uint gidx = __builtin_amdgcn_workitem_id_x() + {threads[0]}u * (__builtin_amdgcn_workitem_id_y() + {threads[1]}u * __builtin_amdgcn_workitem_id_z());')
            in_main = True
            continue
        if not in_main and re.match(r'(int|uint|float|bool|uint64_t|uint2|uint4|float4) x4r_\w+\(.*\) \{', line):
            line = 'X4R_FN ' + line
        line = re.sub(r'\[(branch|loop|unroll|flatten|fastopt)\] ?', '', line)
        line = re.sub(r'\b(float|int|uint)([234])\(', r'X4R_V_\1\2(', line)
        line = re.sub(r'(?<![\w.])(u?int)\(', r'x4r_\1(', line)   # saturating from float (x4r_hip.h)
        out.append(line)
    return f'#include "{os.path.basename(PRELUDE)}"\n' + '\n'.join(out) + '\n'


def build(hip, gpu, hsaco, llvm=None):
    def tool(name):
        for d in [llvm] if llvm else []:
            for n in (name + '.exe', name):
                if os.path.isfile(os.path.join(d, n)):
                    return os.path.join(d, n)
        return name
    # device-only, unbundled: clang links the code object itself
    subprocess.run([tool('clang'), '-x', 'hip', '--cuda-device-only', '--no-gpu-bundle-output', f'--offload-arch={gpu}', '-nogpulib', '-nogpuinc',
                    '-O3', '-std=c++17', '-ffp-contract=off', '-fgpu-flush-denormals-to-zero',   # fp32 denormals flushed, as CM and D3D12 do
                    f'-I{os.path.dirname(PRELUDE)}', '-Wno-deprecated-builtins', '-c', hip, '-o', hsaco],
                   check=True)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    llvm = None
    if '--llvm' in args:
        i = args.index('--llvm'); llvm = args[i + 1]; del args[i:i + 2]
    src, dst = args[0], args[1]
    open(dst, 'w', newline='\n').write(convert(open(src).read()))
    if len(args) >= 5 and args[2] == '--build':
        build(dst, args[3], args[4], llvm)


if __name__ == '__main__':
    main()
