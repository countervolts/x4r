#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Portable project paths shared by development tools.
export X4R_ROOT=${X4R_ROOT:-$(cd "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}
R=$X4R_ROOT
mkdir -p "$R/private/generated/build" "$R/private/generated/out"
for name in build out; do
    if [[ ! -e $R/x4r/$name && ! -L $R/x4r/$name ]]; then
        ln -s "../private/generated/$name" "$R/x4r/$name"
    fi
done
export X4R_SCRATCH=${X4R_SCRATCH:-$R/x4r/build/scratch}
S=$X4R_SCRATCH
mkdir -p "$S"
