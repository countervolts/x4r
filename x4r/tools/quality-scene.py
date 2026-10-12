#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic static 3D quality fixture. Linear RGB, pinhole camera, no tone map.

Ray-cast textured floor/back wall and three shaded spheres. Native reference uses
4x4 regular supersampling; inputs use the same camera and a 32-frame Halton cycle.
This tests spatial reconstruction and static temporal accumulation, not motion.
"""
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "quality" / "scene"
W, H = 2560, 1440


def halton(i, base):
    value, scale = 0., 1.
    while i:
        scale /= base
        value += (i % base) * scale
        i //= base
    return value - .5


def render(w, h, jitter):
    image = np.empty((h, w, 4), np.float32)
    origin = np.array([0., 1.65, -4.5], np.float32)
    light = np.array([-.45, .8, -.4], np.float32)
    light /= np.linalg.norm(light)
    for y0 in range(0, h, 32):
        y1 = min(h, y0 + 32)
        x, y = np.meshgrid((np.arange(w, dtype=np.float32)+.5+jitter[0])/w,
                           (np.arange(y0,y1,dtype=np.float32)+.5+jitter[1])/h)
        d = np.stack([(x-.5)*2*(W/H)*.65, (.5-y)*2*.65-.13, np.ones_like(x)], -1)
        d /= np.linalg.norm(d, axis=-1, keepdims=True)
        # Back wall z=7 and floor y=0.
        t = (7-origin[2])/d[...,2]
        p = origin + t[...,None]*d
        brick = ((np.floor(p[...,0]*2 + (np.floor(p[...,1]*4)%2)*.5)
                  + np.floor(p[...,1]*4)) % 2)
        base = np.stack([.27+.08*brick, .20+.035*brick, .14+.025*brick], -1)
        mortar = ((np.mod(p[...,1]*4,1)<.07) | (np.mod(p[...,0]*2+(np.floor(p[...,1]*4)%2)*.5,1)<.04))
        base[mortar] = [.055,.05,.045]
        tf = np.divide(-origin[1],d[...,1],out=np.full_like(t,1e6),where=d[...,1]<-1e-6)
        floor = (tf>0)&(tf<t)
        pf = origin + tf[...,None]*d
        check = ((np.floor(pf[...,0]*3)+np.floor(pf[...,2]*3))%2)
        floorbase = np.stack([.09+.5*check,.115+.47*check,.14+.4*check],-1)
        # Fine wooden grain and grout exercise high frequency reconstruction.
        floorbase *= (.85+.15*np.sin(pf[...,0]*90+np.sin(pf[...,2]*12)))[...,None]
        base = np.where(floor[...,None],floorbase,base)
        t = np.where(floor,tf,t)
        for center, radius, color in [([-1.15,.85,1.4],.85,[.8,.14,.07]),
                                      ([1.05,.65,2.8],.65,[.06,.3,.75]),
                                      ([.05,1.8,4.2],.45,[.12,.65,.22])]:
            oc = origin-np.array(center,np.float32)
            b = np.sum(d*oc,axis=-1)
            disc = b*b - (np.dot(oc,oc)-radius*radius)
            ts = -b-np.sqrt(np.maximum(disc,0))
            hit = (disc>=0)&(ts>0)&(ts<t)
            ps = origin+ts[...,None]*d
            n = (ps-center)/radius
            shade = .16+.84*np.maximum(np.sum(n*light,axis=-1),0)
            stripe = .7+.3*(np.sin(ps[...,1]*65+ps[...,0]*8)>0)
            c = np.array(color)*shade[...,None]*stripe[...,None]
            spec = np.maximum(np.sum(n*(light-d)/np.linalg.norm(light-d,axis=-1,keepdims=True),axis=-1),0)**48
            c += .3*spec[...,None]
            base = np.where(hit[...,None],c,base)
            t = np.where(hit,ts,t)
        image[y0:y1,:,:3] = np.clip(base,0,1)
        image[y0:y1,:,3] = 1
    return image


def preview(a, path):
    rgb = np.clip(a[...,:3],0,1)
    srgb = np.where(rgb<=.0031308,12.92*rgb,1.055*rgb**(1/2.4)-.055)
    Image.fromarray(np.rint(srgb*255).astype('uint8')).save(path)


if __name__ == '__main__':
    OUT.mkdir(parents=True,exist_ok=True)
    jitters = [[halton(i,2),halton(i,3)] for i in range(1,33)]
    for i,j in enumerate(jitters):
        render(W//2,H//2,j).astype('<f2').tofile(OUT/f'input-{i:02}.rgba16f')
        print(f'input {i}',flush=True)
    ref = np.zeros((H,W,4),np.float32)
    for y in range(4):
        for x in range(4):
            ref += render(W,H,((x+.5)/4-.5,(y+.5)/4-.5))/16
    ref.astype('<f2').tofile(OUT/'reference.rgba16f')
    preview(ref,OUT/'reference.png')
    files = sorted(OUT.glob('*.rgba16f'))
    (OUT/'manifest.json').write_text(json.dumps(dict(width=W,height=H,input_width=W//2,input_height=H//2,
        jitter=jitters,reference_spp=16,scene='static textured room with three spheres',
        color_space='linear RGB; peak=1',sha256={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in files}),indent=2)+'\n')
