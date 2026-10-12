#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Install the validated mixed-dot/WMMA profiles after a baseline DXIL build.
# Two override sets: ovr (= ovr-rdna3, validated on gfx1101) and ovr-rdna4 (no gfx11-only register-layout options).
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
source "$R/x4r/tools/fast-profile.sh"
cd "$R"
free -m
# (the SPIR-V overrides declare the static sampler in set 3 like vkd3d does: bindless.py; with dxc's default set 0 the dyn_2
# family diverged from the DXIL under non-zero motion vectors, 26.1 dB vs 31.0 dB in quality-motion.py)
x4r_fast_profile 2 dxil
bash "$R/x4r/tools/build-kernel.sh" 2
X4R_RDNA=3 X4R_PROFILE=fast bash "$R/x4r/tools/build-wmma.sh" 2 32 34 38 40 42 43 12 7 25 27 28 26 29 13 18 23 33 35 39 41
if [[ ${X4R_SKIP_RDNA4:-0} != 1 ]]; then
  X4R_RDNA=4 X4R_OVR_DIR="$R/prefix/drive_c/x4r/ovr-rdna4" X4R_PROFILE=fast bash "$R/x4r/tools/build-wmma.sh" 2 32 34 38 40 42 43 12 7 25 27 28 26 29 13 18 23 33 35 39 41
fi
