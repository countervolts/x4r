#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# safe.sh <cmd...>: run a heavy tool inside a memory-capped cgroup scope (no swap, 8 GB cap) so it cannot take the machine down
available=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
if [ "$available" -lt 8000 ]; then
    echo "only ${available} MB available, refusing to start" >&2
    exit 9
fi
exec systemd-run --user --scope -q -p MemoryMax=8G -p MemorySwapMax=0 -p CPUQuota=1200% "$@"
