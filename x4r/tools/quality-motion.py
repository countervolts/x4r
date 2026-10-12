#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic moving-camera fixture (the static scene never exercises motion vectors or reprojection).

Same room and spheres as quality-scene.py; the camera pans along x. Writes N jittered 1280x720 inputs, N full-resolution
(2560x1440) motion-vector textures in pixels (XESS_INIT_FLAG_HIGH_RES_MV) and a 2x2-supersampled reference of the last frame.
usage: quality-motion.py [sign]   sign = +1 (default): mv = previous - current position, -1: the opposite convention
"""
import sys
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
W, H, N, STEP = 2560, 1440, 20, 0.05
import os
INW, INH = int(os.environ.get('X4R_IW', W // 2)), int(os.environ.get('X4R_IH', H // 2))   # input resolution (default 2x)
SIGN = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
import os
OUT = ROOT / 'quality' / os.environ.get('X4R_MOTION_DIR', 'motion')
ASPECT = W / H
OBJ = os.environ.get('X4R_OBJ', '')   # '' static scene; 'mv': sphere 0 moves, with correct motion vectors; 'nomv': it moves but its vectors only describe the camera
CUR = [0]
def sphere0(t): return [-1.15 + (0.06 * t if OBJ else 0), .85, 1.4]
CENTERS = [([-1.15,.85,1.4],.85,[.8,.14,.07]), ([1.05,.65,2.8],.65,[.06,.3,.75]), ([.05,1.8,4.2],.45,[.12,.65,.22])]


def halton(i, base):
    v, s = 0., 1.
    while i:
        s /= base; v += (i % base) * s; i //= base
    return v - .5


PATH = os.environ.get('X4R_PATH', 'pan')   # pan: +x only; orbit: x, y and z motion that changes sign (as when running in a game)
def origin_at(t):
    if PATH == 'orbit':
        return np.array([0.09 * np.sin(0.55 * t), 1.65 + 0.03 * np.cos(0.8 * t), -4.5 + 0.015 * t], np.float32)
    return np.array([-0.5 + STEP * t, 1.65, -4.5], np.float32)


def rays(w, h, jitter, y0, y1):
    x, y = np.meshgrid((np.arange(w, dtype=np.float32) + .5 + jitter[0]) / w, (np.arange(y0, y1, dtype=np.float32) + .5 + jitter[1]) / h)
    d = np.stack([(x - .5) * 2 * ASPECT * .65, (.5 - y) * 2 * .65 - .13, np.ones_like(x)], -1)
    return d / np.linalg.norm(d, axis=-1, keepdims=True)


def trace(origin, d):
    """nearest hit distance t and which object (0 wall, 1 floor, 2.. spheres)"""
    t = (7 - origin[2]) / d[..., 2]
    obj = np.zeros(t.shape, np.int8)
    tf = np.divide(-origin[1], d[..., 1], out=np.full_like(t, 1e6), where=d[..., 1] < -1e-6)
    floor = (tf > 0) & (tf < t)
    t = np.where(floor, tf, t); obj[floor] = 1
    for k, (c, r, _) in enumerate(CENTERS):
        c = sphere0(CUR[0]) if k == 0 else c
        oc = origin - np.array(c, np.float32)
        b = np.sum(d * oc, -1); disc = b * b - (np.dot(oc, oc) - r * r)
        ts = -b - np.sqrt(np.maximum(disc, 0))
        hit = (disc >= 0) & (ts > 0) & (ts < t)
        t = np.where(hit, ts, t); obj[hit] = 2 + k
    return t, obj


def shade(origin, d, t, obj):
    p = origin + t[..., None] * d
    brick = ((np.floor(p[..., 0] * 2 + (np.floor(p[..., 1] * 4) % 2) * .5) + np.floor(p[..., 1] * 4)) % 2)
    base = np.stack([.27 + .08 * brick, .20 + .035 * brick, .14 + .025 * brick], -1)
    mortar = ((np.mod(p[..., 1] * 4, 1) < .07) | (np.mod(p[..., 0] * 2 + (np.floor(p[..., 1] * 4) % 2) * .5, 1) < .04))
    base[mortar] = [.055, .05, .045]
    check = ((np.floor(p[..., 0] * 3) + np.floor(p[..., 2] * 3)) % 2)
    fb = np.stack([.09 + .5 * check, .115 + .47 * check, .14 + .4 * check], -1) * (.85 + .15 * np.sin(p[..., 0] * 90 + np.sin(p[..., 2] * 12)))[..., None]
    base = np.where((obj == 1)[..., None], fb, base)
    light = np.array([-.45, .8, -.4], np.float32); light /= np.linalg.norm(light)
    for k, (c, r, col) in enumerate(CENTERS):
        c = sphere0(CUR[0]) if k == 0 else c
        n = (p - np.array(c, np.float32)) / r
        sh = .16 + .84 * np.maximum(np.sum(n * light, -1), 0)
        stripe = .7 + .3 * (np.sin(p[..., 1] * 65 + p[..., 0] * 8) > 0)
        cc = np.array(col) * sh[..., None] * stripe[..., None]
        spec = np.maximum(np.sum(n * (light - d) / np.linalg.norm(light - d, axis=-1, keepdims=True), -1), 0) ** 48
        cc = cc + .3 * spec[..., None]
        base = np.where((obj == 2 + k)[..., None], cc, base)
    return np.clip(base, 0, 1)


def render(origin, w, h, jitter):
    img = np.empty((h, w, 4), np.float32)
    for y0 in range(0, h, 32):
        y1 = min(h, y0 + 32)
        d = rays(w, h, jitter, y0, y1)
        t, obj = trace(origin, d)
        img[y0:y1, :, :3] = shade(origin, d, t, obj); img[y0:y1, :, 3] = 1
    return img


def lowres_inputs(t, jitter):
    """depth (inverted: near = 1) and motion (input pixels, previous - current) on the jittered input grid"""
    w, h = INW, INH
    o = origin_at(t)
    depth = np.empty((h, w), np.float32); mv = np.zeros((h, w, 2), np.float32)
    op = origin_at(t - 1) if t else o
    for y0 in range(0, h, 32):
        y1 = min(h, y0 + 32)
        d = rays(w, h, jitter, y0, y1)
        tt, ob = trace(o, d)
        depth[y0:y1] = 1.0 / (1.0 + tt)
        if t:
            pos = o + tt[..., None] * d
            if OBJ == 'mv': pos = pos - (ob == 2)[..., None] * (np.array(sphere0(t)) - np.array(sphere0(t - 1)))[None, None, :]
            u = pos - op
            xp = (.5 + (u[..., 0] / u[..., 2]) / (2 * ASPECT * .65)) * w
            yp = (.5 - (u[..., 1] / u[..., 2] + .13) / 1.3) * h
            xc = (np.arange(w, dtype=np.float32) + .5 + jitter[0])[None, :]
            yc = (np.arange(y0, y1, dtype=np.float32) + .5 + jitter[1])[:, None]
            mv[y0:y1, :, 0] = SIGN * (xp - xc); mv[y0:y1, :, 1] = SIGN * (yp - yc)
    return depth, mv


def motion(t):
    """pixels, previous - current screen position of the surface under each output pixel (zeros for frame 0)"""
    mv = np.zeros((H, W, 2), np.float32)
    if t == 0: return mv
    o, op = origin_at(t), origin_at(t - 1)
    for y0 in range(0, H, 32):
        y1 = min(H, y0 + 32)
        d = rays(W, H, (0, 0), y0, y1)
        tt, ob = trace(o, d)
        pos = o + tt[..., None] * d
        if OBJ == 'mv': pos = pos - (ob == 2)[..., None] * (np.array(sphere0(t)) - np.array(sphere0(t - 1)))[None, None, :]
        u = pos - op
        xp = (.5 + (u[..., 0] / u[..., 2]) / (2 * ASPECT * .65)) * W
        yp = (.5 - (u[..., 1] / u[..., 2] + .13) / 1.3) * H
        xc = (np.arange(W, dtype=np.float32) + .5)[None, :]
        yc = (np.arange(y0, y1, dtype=np.float32) + .5)[:, None]
        mv[y0:y1, :, 0] = SIGN * (xp - xc); mv[y0:y1, :, 1] = SIGN * (yp - yc)
    return mv


if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    for t in range(N):
        CUR[0] = t
        j = (halton(t + 1, 2), halton(t + 1, 3))
        render(origin_at(t), INW, INH, j).astype('<f2').tofile(OUT / f'input-{t:02}.rgba16f')
        m = motion(t); m.astype('<f2').tofile(OUT / f'mv-{t:02}.rg16f')
        dep, mvl = lowres_inputs(t, j)
        dep.astype('<f4').tofile(OUT / f'depth-{t:02}.r32f'); mvl.astype('<f2').tofile(OUT / f'mvlo-{t:02}.rg16f')
        print(f'frame {t} mv range x[{m[...,0].min():.2f},{m[...,0].max():.2f}] y[{m[...,1].min():.2f},{m[...,1].max():.2f}]', flush=True)
    CUR[0] = N - 1
    ref = np.zeros((H, W, 4), np.float32)
    for yy in range(2):
        for xx in range(2):
            ref += render(origin_at(N - 1), W, H, ((xx + .5) / 2 - .5, (yy + .5) / 2 - .5)) / 4
    ref.astype('<f2').tofile(OUT / 'reference.rgba16f')
    print('done')
