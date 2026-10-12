#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rewrite OpenCL convert_<type>N(<vec>) calls (spir_func, mangled) into plain LLVM casts so opt can fold the i64 address math."""
import re, sys
src = open(sys.argv[1]).read()
SRC = {'c': ('i8', True), 's': ('i16', True), 'i': ('i32', True), 'l': ('i64', True),
       'h': ('i8', False), 't': ('i16', False), 'j': ('i32', False), 'm': ('i64', False), 'f': ('float', None), 'Dh': ('half', None)}
DST = {'int': 'i32', 'uint': 'i32', 'long': 'i64', 'ulong': 'i64', 'short': 'i16', 'ushort': 'i16', 'char': 'i8', 'uchar': 'i8', 'float': 'float', 'half': 'half'}
BITS = {'i8': 8, 'i16': 16, 'i32': 32, 'i64': 64, 'half': 16, 'float': 32}
n = [0]
def repl(m):
    name, vty, arg = m.group(2), m.group(3), m.group(4)   # name = convert_xxxN, call result type, arg
    mm = re.match(r'_Z\d+convert_([a-z]+?)(\d+)Dv\d+_(Dh|[a-z])$', name)
    if not mm: return m.group(0)
    d, cnt, s = mm.group(1), mm.group(2), mm.group(3)
    dt, (st, sg) = DST[d], SRC[s]
    res = m.group(1)
    vt = lambda t: f'<{cnt} x {t}>'
    dty, sty = vt(dt), vt(st)
    if dt == st: return f'{res} = bitcast {sty} {arg} to {dty}'
    fl = lambda t: t in ('float', 'half')
    if fl(dt) and fl(st): op = 'fpext' if BITS[dt] > BITS[st] else 'fptrunc'
    elif fl(dt): op = 'sitofp' if sg else 'uitofp'
    elif fl(st): op = 'fptoui' if d.startswith('u') else 'fptosi'
    elif BITS[dt] > BITS[st]: op = 'sext' if sg else 'zext'
    else: op = 'trunc'
    return f'{res} = {op} {sty} {arg} to {dty}'
pat = re.compile(r'(%[\w.]+) = (?:tail )?call spir_func (<\d+ x \w+>) @(_Z\d+convert_\w+)\((<\d+ x \w+>) ([^)]*)\)[^\n]*')
def repl2(m):
    res, rty, name, aty, arg = m.groups()
    mm = re.match(r'_Z\d+convert_([a-z]+?)(\d+)Dv\d+_(Dh|[a-z])$', name)
    if not mm: return m.group(0)
    d, cnt, s = mm.groups()
    dt, (st, sg) = DST[d], SRC[s]
    fl = lambda t: t in ('float', 'half')
    if dt == st: return f'{res} = bitcast {aty} {arg} to {rty}'
    if fl(dt) and fl(st): op = 'fpext' if BITS[dt] > BITS[st] else 'fptrunc'
    elif fl(dt): op = 'sitofp' if sg else 'uitofp'
    elif fl(st): op = 'fptoui' if d.startswith('u') else 'fptosi'
    elif BITS[dt] > BITS[st]: op = 'sext' if sg else 'zext'
    else: op = 'trunc'
    return f'{res} = {op} {aty} {arg} to {rty}'
open(sys.argv[2], 'w').write(pat.sub(repl2, src))
