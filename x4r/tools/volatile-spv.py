#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Experiment: mark uniform/storage-buffer OpLoad instructions Volatile.

This strengthens memory access semantics without changing the operations. It
can reduce load hoisting and register lifetimes. Validate the resulting module
and measure it; it may be substantially slower. Workgroup/image loads stay intact.
"""
import struct
import sys
from pathlib import Path

src, dst = map(Path,sys.argv[1:3])
data = src.read_bytes()
words = list(struct.unpack(f'<{len(data)//4}I',data))
if words[0] != 0x07230203:
    raise ValueError('Not SPIR-V')
instructions = []
pos = 5
while pos < len(words):
    count = words[pos] >> 16
    if not count or pos+count > len(words):
        raise ValueError('Invalid instruction length')
    instructions.append(words[pos:pos+count])
    pos += count
pointers = {i[1]:i[2] for i in instructions if i[0]&65535 == 32}
pointer_ids = {}
for i in instructions:
    if i[0]&65535 in (46,59,65,66,67,70,83,124,245) and len(i)>=3 and i[1] in pointers:
        pointer_ids[i[2]] = pointers[i[1]]
changed = 0
for i in instructions:
    if i[0]&65535 == 61 and pointer_ids.get(i[3]) in (2,12):
        if len(i)==4:
            i.append(1)
            i[0] = (len(i)<<16)|61
        else:
            i[4] |= 1
        changed += 1
flat = words[:5]+[w for i in instructions for w in i]
dst.write_bytes(struct.pack(f'<{len(flat)}I',*flat))
print(f'{changed} buffer loads marked Volatile')
