#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# build-all.sh: translate and compile captured kernels sequentially.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
[[ ${1:-1} == 1 ]] || { echo 'Parallel builds are disabled; use build-all.sh without a job count.' >&2; exit 2; }
mkdir -p "$R/x4r/out"
for i in $(awk '{print $1}' "$R/kernels/map.txt"); do
    free -m
    "$R/x4r/tools/build-kernel.sh" "$i" > "$R/x4r/out/build_$i.log" 2>&1 || {
        echo "kernel $i failed; see x4r/out/build_$i.log" >&2
        exit 1
    }
done
echo 'All captured kernels built'
