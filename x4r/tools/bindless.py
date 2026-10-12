#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# bindless.py <in.hlsl> <rootsig.txt> <out.hlsl> : rewrite x4rwave's flat resource declarations into the bindless layout vkd3d-proton uses
# (push constant word p = descriptor index of root parameter p; SSBO/CBV runtime arrays in set 2 binding 0)
import re, sys
src, rsf, dst = sys.argv[1:4]
par = {}; npar = 0
for l in open(rsf):
    if l.startswith('param '): npar += 1
    m = re.match(r'param (\d+) table (SRV|UAV|CBV):(\d+):', l)
    if m: par[(m.group(2), int(m.group(3)))] = int(m.group(1))
out = ["struct X4RPC { uint w[%d]; };" % npar, "[[vk::push_constant]] X4RPC x4rpc;",
       "struct X4RCB { uint4 d[4096]; };",
       "[[vk::binding(0, 2)]] ByteAddressBuffer X4R_SRV[];",
       "[[vk::binding(0, 2)]] RWByteAddressBuffer X4R_UAV[];",
       "[[vk::binding(0, 2)]] ConstantBuffer<X4RCB> X4R_CBV[];",
       "[[vk::binding(1, 1)]] Texture2D<float4> X4R_TEX[];",
       "[[vk::binding(1, 1)]] [[vk::image_format(\"unknown\")]] RWTexture2D<float4> X4R_RWTEX[];"]
body = []
for l in open(src):
    m = re.match(r'(Texture2D<float4>|RWTexture2D<float4>|ByteAddressBuffer|RWByteAddressBuffer) R(\d+) : register\(([tu])(\d+)\);', l)
    if m:
        k = 'SRV' if m.group(3) == 't' else 'UAV'
        a = ('X4R_TEX' if k == 'SRV' else 'X4R_RWTEX') if 'Texture' in m.group(1) else 'X4R_' + k
        out.append("#define R%s %s[x4rpc.w[%d]]" % (m.group(2), a, par[(k, int(m.group(4)))]))
        continue
    m = re.match(r'cbuffer CB(\d+) : register\(b(\d+)\) \{ uint4 CBD\d+\[4096\]; \};', l)
    if m:
        out.append("#define CBD%s X4R_CBV[x4rpc.w[%d]].d" % (m.group(1), par[('CBV', int(m.group(2)))]))
        continue
    body.append(l.rstrip("\n"))
# declarations must precede first use: place after the helper prelude's first line
open(dst, "w").write("\n".join(out + body) + "\n")
