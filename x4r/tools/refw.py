#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""refw.py <dump.bin> : numpy reference for the two-thread WMMA dpas prototype (vkrun seed 12345 inputs); prints the number of mismatching accumulator words"""
import numpy as np, struct, sys
M=0xffffffff
rs=12345
def rnd():
    global rs
    rs^=(rs<<13)&M; rs^=rs>>17; rs^=(rs<<5)&M
    return rs
need=1000300
buf=np.zeros(need,dtype=np.uint32)
for k in range(need):
    r=rnd()
    v=struct.unpack('<I',struct.pack('<f',(int(r&0xffff)-32768)/16384.0))[0]
    if rnd()&1: v=r
    buf[k]=v
def i8(x): return np.array([(x>>(8*q))&0xff for q in range(4)],dtype=np.int64).astype(np.int8).astype(np.int64)
out=np.fromfile(sys.argv[1],dtype=np.uint32)
bad=0;tot=0
for wg in (0,1,7):
    for t in (0,1):
        a=[[int(buf[wg*64+s*32+16*t+l]) for l in range(16)] for s in range(4)]
        b=[[int(buf[1000000+l+s*32]) for l in range(16)] for s in range(8)]
        acc=np.zeros((8,16),dtype=np.int64)
        for it in range(256):
            A=np.zeros((8,32),dtype=np.int64)
            for s in range(4):
                for l in range(16):
                    w=16*s+l; r=w>>3; d=w&7
                    A[r,4*d:4*d+4]=i8((a[s][l]+(it if s==0 else 0))&M)
            B=np.zeros((32,16),dtype=np.int64)
            for s in range(8):
                for n_ in range(16):
                    B[4*s:4*s+4,n_]=i8((b[s][n_]+(3*it if s==0 else 0))&M)
            acc+=A@B
        for r in range(8):
            for l in range(16):
                tot+=1
                if int(out[wg*256+r*32+16*t+l])!=(int(acc[r,l])&M): bad+=1
print(sys.argv[1],"mismatching accumulator words:",bad,"of",tot)
