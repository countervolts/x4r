#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""quality-motion-compare.py <label=dir>...: PSNR of each run's last frame against the supersampled reference and against the first run."""
import math, sys
from pathlib import Path
import numpy as np
W, H = 2560, 1440
import os
root = Path(__file__).resolve().parents[2] / 'quality' / os.environ.get('X4R_MOTION_DIR', 'motion')
def load(p):
    return np.fromfile(p, dtype='<f2').reshape(H, W, 4)[..., :3].astype(np.float64)
def srgb(a):
    a = np.clip(a, 0, 1); return np.where(a <= .0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - .055)
def psnr(a, b):
    m = float(np.mean((a - b) ** 2)); return 99.0 if m == 0 else -10 * math.log10(m)
ref = load(root / 'reference.rgba16f')
runs = [(a.split('=')[0], load(Path(a.split('=')[1]) / 'frame-019.rgba16f')) for a in sys.argv[1:]]
base = runs[0][1]
for name, img in runs:
    print(f'{name:10s} vs reference: {psnr(srgb(img), srgb(ref)):6.2f} dB sRGB   vs {runs[0][0]}: {psnr(srgb(img), srgb(base)):6.2f} dB')
