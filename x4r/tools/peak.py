#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""peak.py cmd...: run a command and print its peak RSS (via wait4) - run under safe.sh"""
import os, sys, resource, time
t=time.time(); pid=os.fork()
if pid==0: os.execvp(sys.argv[1], sys.argv[1:])
_,st,ru=os.wait4(pid,0)
print("exit %d, peak RSS %.2f GB, %.1fs"%(os.waitstatus_to_exitcode(st), ru.ru_maxrss/1048576, time.time()-t), file=sys.stderr)
