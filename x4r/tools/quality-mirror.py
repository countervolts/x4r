#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""quality-mirror.py <src fixture dir> <dst fixture dir>: mirror a motion fixture left-to-right (colour, depth, motion x negated,
reference). Run the harness with XESS_JXSIGN=-1 so the jitter is mirrored too. A convolutional upscaler should treat the mirrored
scene like the original; a direction-dependent difference points at a coordinate/sign error."""
import sys
from pathlib import Path
import numpy as np
src, dst = Path(sys.argv[1]), Path(sys.argv[2]); dst.mkdir(parents=True, exist_ok=True)
W, H = 2560, 1440
def flip(a): return np.ascontiguousarray(a[:, ::-1])
for f in sorted(src.iterdir()):
    n = f.name
    if n.startswith('input-'):
        a = np.fromfile(f, '<f2').reshape(-1, 4); h = a.size // 4 // (W // 3 if False else 1)  # shape from file size below
        iw = int(__import__('os').environ.get('X4R_IW', 1280)); ih = a.shape[0] // iw
        flip(a.reshape(ih, iw, 4)).astype('<f2').tofile(dst / n)
    elif n.startswith('depth-'):
        a = np.fromfile(f, '<f4'); iw = int(__import__('os').environ.get('X4R_IW', 1280)); ih = a.size // iw
        flip(a.reshape(ih, iw)).astype('<f4').tofile(dst / n)
    elif n.startswith('mvlo-') or n.startswith('mv-'):
        a = np.fromfile(f, '<f2'); iw = (int(__import__('os').environ.get('X4R_IW', 1280)) if n.startswith('mvlo') else W); ih = a.size // 2 // iw
        m = flip(a.reshape(ih, iw, 2).astype(np.float32)); m[..., 0] *= -1; m.astype('<f2').tofile(dst / n)
    elif n == 'reference.rgba16f':
        flip(np.fromfile(f, '<f2').reshape(H, W, 4)).astype('<f2').tofile(dst / n)
print('mirrored', len(list(dst.iterdir())), 'files')
