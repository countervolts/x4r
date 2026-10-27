#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# After playing a few seconds with XeSS on:  bash x4r/x4r-check.sh
# Reads x4r/status.txt, which the shim rewrites while XeSS runs, and says whether the translated Intel XMX kernels executed.
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
S=$HERE/status.txt
[[ -f $S ]] || { echo 'FAIL: no x4r/status.txt - the x4r shim was never loaded. Start the game and enable XeSS (DX12).'; exit 1; }
get() { sed -n "s/^$1=//p" "$S"; }
age=$(( $(date +%s) - $(stat -c %Y "$S") ))
echo "XeSS library : $(get xess_library)"
echo "GPU          : $(get gpu) (detected RDNA generation: $(get rdna); 0 = not RDNA3/4)"
echo "Kernel set   : $(get kernel_set)"
echo "Kernels      : $(get translated_kernels) translated, $(get wmma_kernels) on WMMA, $(get failed_kernels) failed"
echo "Dispatches   : $(get dispatches)   XeSS GPU time/frame: $(get xess_gpu_ms_per_frame) ms"
echo "Last update  : ${age}s ago"
if [[ -f $HERE/frame-sync-status.txt ]]; then
  echo 'Frame-sync trial:'
  sed -n '/^frame_sync_/p' "$HERE/frame-sync-status.txt"
fi
case $(get result) in
  OK) echo 'PASS: XeSS ran its Intel XMX network through x4r kernels (matrix work on AMD WMMA; no Intel XMX hardware).'
      (( $(get wmma_kernels) > 0 )) || echo 'note: no WMMA kernels in use (stock dot4 set): correct but slower.'; exit 0 ;;
  PENDING) echo 'PENDING: x4r loaded but XeSS has not dispatched yet. Enable XeSS in the game and run again.'; exit 2 ;;
  *) echo 'FAIL: kernels failed (wrong libxess.dll version or untranslated kernels). See C:\igdext_trace.log with IGDEXT_TRACE=1.'; exit 1 ;;
esac
