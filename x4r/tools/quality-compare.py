#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""RGB PSNR at peak 1 in linear space and separately in sRGB display space."""
import json
import hashlib
import math
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2] / 'quality'
W, H = 2560, 1440


def load(path):
    a = np.fromfile(path,dtype='<f2')
    if a.size != W*H*4:
        raise ValueError(f'{path}: incorrect dimensions')
    a = a.reshape(H,W,4)[...,:3].astype(np.float32)
    if not np.isfinite(a).all():
        raise ValueError(f'{path}: nonfinite pixels')
    return a


def srgb(a):
    a = np.clip(a,0,1)
    return np.where(a<=.0031308,12.92*a,1.055*a**(1/2.4)-.055)


def metrics(a,b):
    d = a.astype(np.float64)-b
    mse = float(np.mean(d*d))
    return dict(mse=mse,psnr_db=(-10*math.log10(mse) if mse else None),
                max_abs=float(np.max(np.abs(d))),mae=float(np.mean(np.abs(d))),
                equal_elements=int(np.count_nonzero(a==b)),elements=int(a.size))


def preview(a,path):
    Image.fromarray(np.rint(srgb(a)*255).astype('uint8')).save(path)


ref = load(ROOT/'scene/reference.rgba16f')
rows = []
for frame in (31,63,127):
    outputs = {m:load(ROOT/m/f'frame-{frame:03}.rgba16f') for m in ('dp4a','dot4','wmma')}
    for mode,a in outputs.items():
        rows.append(dict(frame=frame+1,comparison=f'{mode} vs native 16spp',
                         linear=metrics(a,ref),srgb=metrics(srgb(a),srgb(ref)),
                         outside_unit_range=int(np.count_nonzero((a<0)|(a>1)))))
        if frame==127:
            preview(a,ROOT/mode/'frame-127.png')
    for a,b in [('wmma','dp4a'),('dot4','dp4a'),('wmma','dot4')]:
        rows.append(dict(frame=frame+1,comparison=f'{a} vs {b}',
                         linear=metrics(outputs[a],outputs[b]),srgb=metrics(srgb(outputs[a]),srgb(outputs[b]))))
    if frame==127:
        diff = np.clip(np.abs(outputs['wmma']-outputs['dot4'])*20,0,1)
        Image.fromarray(np.rint(diff*255).astype('uint8')).save(ROOT/'wmma-dot4-diff20.png')
report = dict(width=W,height=H,quality='Performance',input_resolution=[1280,720],
              legacy_scale=True,
              protocol='Static scene, same 32 Halton inputs repeated, history reset on frame 1, RGB only, peak=1',
              limitation='One procedural 3D scene; no motion, disocclusion, game renderer, or native Intel XMX comparison',
              srgb_metric='Clamp [0,1] then IEC sRGB transfer; linear metric uses unclamped readbacks',results=rows)
paths=[ROOT/'scene/manifest.json',ROOT/'scene/reference.rgba16f']
for mode in ('dp4a','dot4','wmma'):
    paths += [ROOT/mode/'run.log',*sorted((ROOT/mode).glob('frame-*.rgba16f'))]
report['capture_sha256']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
(ROOT/'results.json').write_text(json.dumps(report,indent=2)+'\n')
for r in rows:
    l,s = r['linear']['psnr_db'],r['srgb']['psnr_db']
    print(f"frame {r['frame']:3} {r['comparison']:24} linear {l if l is not None else 'inf'} dB, sRGB {s if s is not None else 'inf'} dB")
