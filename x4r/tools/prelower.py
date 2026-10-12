#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rewrite genx.vload/vstore (documented as exactly LLVM load/store) into plain load/store so opt can SROA them."""
import re, sys
src = open(sys.argv[1]).read()
src = re.sub(r'call (<\d+ x [a-z0-9]+>) @llvm\.genx\.vload\.[\w.]+\(ptr (?:nonnull )?(%[\w.]+)\)( #\d+)?',
             r'load \1, ptr \2', src)
src = re.sub(r'call void @llvm\.genx\.vstore\.[\w.]+\((<\d+ x [a-z0-9]+>) ([^,]+), ptr (?:nonnull )?(%[\w.]+)\)( #\d+)?',
             r'store \1 \2, ptr \3', src)
open(sys.argv[2], 'w').write(src)
