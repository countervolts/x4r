#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# stats.sh <dyn id...>: RADV shader statistics (VGPRs, spills, instruction count) of the current translation
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; S="$X4R_SCRATCH"
export LD_LIBRARY_PATH=$R/deps/dl-dxc/lib
for i in "$@"; do
  h=$(awk -v i=$i '$1==i{print $2}' $R/kernels/map.txt)
  $R/x4r/tools/safe.sh $R/deps/dl-dxc/bin/dxc -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.3 -HV 2021 -enable-16bit-types -O0 $R/x4r/out/$h.hlsl -Fo $S/st_$i.spv 2>&1 | grep -i " error" | head -2
  echo -n "dyn_$i: "; RADV_DEBUG=shaderstats $R/x4r/tools/safe.sh $R/x4r/build/vkpipe $S/st_$i.spv 2>&1 | grep -E "^(SGPRs|VGPRs|Spilled VGPRs|Scratch size|Instructions|Subgroups per SIMD|Latency):" | sed 's/: /=/' | paste -sd' '
done
