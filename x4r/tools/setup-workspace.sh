#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Create ignored private data directories and legacy paths used by the tools.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
mkdir -p "$R/private"
for name in prefix kernels deps vendor third_party shots quality; do
    if [[ -e $R/$name && ! -L $R/$name ]]; then
        echo "$R/$name already exists; leaving it in place" >&2
        continue
    fi
    mkdir -p "$R/private/$name"
    [[ -L $R/$name ]] || ln -s "private/$name" "$R/$name"
done
echo "Private workspace ready at $R/private"
