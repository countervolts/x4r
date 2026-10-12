#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# lift-kernel.sh <dyn id>... : captured CM SPIR-V (kernels/spv/dyn_N.spv) -> LLVM IR with genx intrinsics (kernels/ll/dyn_N.x.ll),
# the input of build-kernel.sh / build-wmma.sh. Reproduces the existing dyn_0..29 files byte-for-byte (apart from the module name).
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
L=$R/kernels/ll; mkdir -p "$L"
for i in "$@"; do
  llvm-spirv -r "$R/kernels/spv/dyn_$i.spv" -o "$L/dyn_$i.bc"
  llvm-dis "$L/dyn_$i.bc" -o "$L/dyn_$i.ll"
  python3 "$R/x4r/tools/prelower.py" "$L/dyn_$i.ll" "$L/dyn_$i.pre.ll"
  opt -S -O2 "$L/dyn_$i.pre.ll" -o "$L/dyn_$i.opt.ll"
  python3 "$R/x4r/tools/postlower.py" "$L/dyn_$i.opt.ll" "$L/dyn_$i.p.ll"
  opt -S -passes=instcombine "$L/dyn_$i.p.ll" -o "$L/dyn_$i.x.ll"
  echo "dyn_$i lifted: $(grep -c 'llvm.genx.dpas2' "$L/dyn_$i.x.ll") dpas2 calls"
done
