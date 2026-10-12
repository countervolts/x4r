#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# isahist.sh <id>: instruction histogram of the final GPU code (HLSL -> DXIL -O0 -> dxil-spirv -> RADV), same path as the real pipeline
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"; S="$X4R_SCRATCH"
i=$1; DXC_OPT=${DXC_OPT:--O0} $R/x4r/tools/vkstats.sh $i
RADV_DEBUG=shaders $R/x4r/tools/safe.sh $R/x4r/build/vkpipe $S/vs_$i.spv > $S/vs_$i.isa 2>&1
grep -E "^\s+(v_|s_|ds_|scratch_|buffer_|global_|flat_|image_)" $S/vs_$i.isa | awk '{print $1}' | sort | uniq -c | sort -rn | head -${TOP:-22} | awk '{printf "%s %s | ", $1, $2} END {print ""}' | fold -w 150
