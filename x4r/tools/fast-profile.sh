#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Selected per-kernel settings, validated on gfx1101 at 1440p Performance.
# X4R_RDNA=3 (default) targets gfx11; X4R_RDNA=4 (gfx12) drops the options that assume RADV's gfx11 register layout
# for cooperative-matrix accumulators (REGC) and keeps everything the Vulkan spec defines (LDS-staged tiles).
x4r_fast_profile() {
  local kernel=$1 target=${2:-spirv} rdna=${X4R_RDNA:-3}
  unset X4R_WORD_GATHER X4R_EFORM_MIN X4R_FWD_DEPTH X4R_NATIVE_MIXED_DOT
  unset X4R_PACK_EARLY X4R_WMMA_SINGLE X4R_WMMA_REGC X4R_WMMA_REGA X4R_WMMA_REGB
  unset X4R_WMMA_SCATTERA X4R_WMMA_ASTRIDE X4R_WMMA_BSTRIDE X4R_REG_ROUTE
  export X4R_SIMPLE_SHUFFLES=1 X4R_BITCAST_FWD=1 X4R_FWD=3 X4R_WMMA_CACHE=6
  case "$kernel" in
    2)
      export X4R_WORD_GATHER=1 X4R_EFORM_MIN=5 X4R_FWD_DEPTH=128
      [[ $target != spirv ]] || export X4R_NATIVE_MIXED_DOT=1
      ;;
    29)
      export X4R_WMMA_SCATTERA=1 X4R_WMMA_SINGLE=1 X4R_WMMA_CACHE=9
      [[ $rdna != 3 ]] || export X4R_WMMA_REGC=1      # row=2*component+tt, col=lane is the gfx11 wave32 layout only
      ;;
    7) export X4R_WMMA_SCATTERA=1 ;;
    26) export X4R_WMMA_SCATTERA=1 X4R_PACK_EARLY=1 X4R_WMMA_CACHE=9 ;;
    12|13|18|23|25|27|28) export X4R_PACK_EARLY=1 ;;
    *) echo "No validated fast profile for dyn_$kernel" >&2; return 2 ;;
  esac
  case "$kernel" in
    7|26|29) export X4R_WMMA_ASTRIDE=12 X4R_WMMA_BSTRIDE=12 ;;
  esac
}
