#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Build the project-owned tools, one compiler job at a time.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
cd "$R"
free -m
mkdir -p x4r/build
read -ra cxxflags <<< "$(llvm-config --cxxflags | sed 's/-std=c++[0-9]*//;s/-O[0-9]//;s/-fno-exceptions//')"
read -ra ldflags <<< "$(llvm-config --ldflags | tr '\n' ' ')"
read -ra libs <<< "$(llvm-config --libs core irreader support analysis)"
for tool in x4rwave x4rsim x4rtrans x4rdep x4rpress; do
    standard=c++17
    [[ $tool != x4rsim ]] || standard=c++20
    x4r/tools/safe.sh g++ "-std=$standard" -O1 -pthread "${cxxflags[@]}" "x4r/src/$tool.cpp" \
        -o "x4r/build/$tool" "${ldflags[@]}" "${libs[@]}"
done
for tool in vkrun vkpipe coopq; do
    x4r/tools/safe.sh gcc -O1 "x4r/tools/$tool.c" -o "x4r/build/$tool" -lvulkan
done
