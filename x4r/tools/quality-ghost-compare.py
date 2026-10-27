#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare the moving-sphere fixture: scene output.json label=capture ..."""
import json
import sys
from pathlib import Path
import numpy as np
from scipy.ndimage import maximum_filter
from PIL import Image

W, H = 2560, 1440
scene, output = Path(sys.argv[1]), Path(sys.argv[2])
def load(path):
    return np.fromfile(path, dtype='<f2').reshape(H, W, 4)[..., :3].astype(np.float32)
def srgb(a):
    a = np.clip(a, 0, 1)
    return np.where(a <= .0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - .055)
def red(a):
    return (a[..., 0] > 1.8 * a[..., 1]) & (a[..., 0] > .08)
def psnr(a, b, mask=None):
    delta = a - b
    if mask is not None:
        delta = delta[mask]
    mse = float(np.mean(delta * delta, dtype=np.float64))
    return 99.0 if mse == 0 else float(-10 * np.log10(mse))

ref_linear = load(scene / 'reference.rgba16f')
ref = srgb(ref_linear)
raw = np.fromfile(scene / 'input-00.rgba16f', dtype='<f2')
ih = 480 if raw.size == 856 * 480 * 4 else 720
iw = raw.size // (ih * 4)
first = red(raw.reshape(ih, iw, 4)[..., :3])
first = np.array(Image.fromarray(first.astype(np.uint8) * 255).resize((W, H), Image.Resampling.NEAREST)) > 0
now = red(ref_linear)
vacated = first & ~maximum_filter(now, size=25)
ring = maximum_filter(now, size=81) & ~maximum_filter(now, size=9)
result = {'scene': str(scene), 'frame': 19, 'vacated_pixels': int(vacated.sum()), 'runs': {}}
ys, xs = np.where(first | now)
crop = (slice(max(0, ys.min() - 40), min(H, ys.max() + 40)), slice(max(0, xs.min() - 80), min(W, xs.max() + 40)))
tiles = [ref[crop]]
for arg in sys.argv[3:]:
    label, path = arg.split('=', 1)
    linear = load(Path(path) / 'frame-019.rgba16f')
    assert np.isfinite(linear).all(), f'{label}: nonfinite output'
    image = srgb(linear)
    redness = lambda a: a[..., 0] - .5 * (a[..., 1] + a[..., 2])
    result['runs'][label] = {
        'whole_srgb_psnr': psnr(image, ref),
        'vacated_srgb_psnr': psnr(image, ref, vacated),
        'ring_srgb_psnr': psnr(image, ref, ring),
        'vacated_red_excess': float(np.mean((redness(image) - redness(ref))[vacated])),
    }
    tiles.append(image[crop])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(result, indent=2) + '\n')
Image.fromarray((np.clip(np.concatenate(tiles, axis=1), 0, 1) * 255).astype(np.uint8)).save(output.with_suffix('.png'))
print(json.dumps(result, indent=2))
