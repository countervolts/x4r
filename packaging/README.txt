x4r - Intel XeSS XMX path on AMD RDNA3 / RDNA4 (Linux, Proton)
===============================================================

Runs XeSS' XMX (matrix) network on AMD GPUs by translating Intel's kernels to HLSL; the matrix work runs on RDNA's
WMMA units. Supported: Radeon RX 7000 and RX 9000 series (RDNA3: gfx1100-1103, gfx1150/1151; RDNA4: gfx1200/1201)
through Mesa RADV. Any other AMD GPU falls back to the stock dot4 kernels, or to XeSS' normal DP4a mode.

Install
-------
1. Extract this zip into the folder that holds the game's main .exe (Unreal Engine: <Game>/Binaries/Win64).
   You should end up with igdext64.dll and an x4r folder next to the .exe.
2. Launch the game once with Proton so its prefix exists, close it, then run (one time):
       bash x4r/install.sh
   Steam games are found automatically. For another launcher: bash x4r/install.sh --prefix /path/to/prefix
3. Use GE-Proton 11 (vkd3d-proton 3.x). Select XeSS in the game's DX12 renderer. No launch options are needed.

What install.sh changes: it copies igdext64.dll into the prefix's windows/system32 (Proton fixes the path XeSS loads it
from) keeping Proton's file as igdext64.dll.x4r-backup, and adds an x4r block to <game>/dxvk.conf that makes the GPU
report an Intel vendor/device id (XeSS only enables its XMX path on Intel). Nothing else is touched; no sudo.
Run it again after Proton updates the prefix.

Uninstall: bash x4r/uninstall.sh, then delete igdext64.dll and the x4r folder.

Verify: play a few seconds with XeSS on, then run  bash x4r/x4r-check.sh . It reads x4r/status.txt, written by the shim
while XeSS runs, and prints PASS only if translated Intel XMX kernels were dispatched (and how many run on WMMA).
x4r does not fall back silently: if a kernel cannot be run, XeSS pipeline creation fails and the check prints FAIL.

Settings: x4r/x4r.ini (Profile = auto | rdna3 | rdna4 | dxil). Logs: set IGDEXT_TRACE=1 in the launch options to get
C:\igdext_trace.log inside the prefix; it says which GPU generation and kernel set were chosen.

Limits
------
* Kernels are translated from XeSS 2.0.2.68 only. A game with another libxess.dll makes XeSS fail rather than
  silently using DP4a; x4r-check.sh reports FAIL. Use XeSS' own DP4a by removing the x4r install.
* Tested: Radeon RX 7700 XT (gfx1101), Wine + vkd3d-proton, synthetic fixture. The RDNA4 kernel set, other RDNA3
  GPUs, real games and motion/disocclusion quality are untested. Report results in the issue tracker.
* Frame generation is not translated. Experimental research prototype, not affiliated with Intel or AMD.

x4r is GPL-3.0-only (LICENSE). The shim is built from xess-xmx-linux / dxvk-igdext (MIT, licenses/). The translated
kernels derive from Intel's XeSS and are not covered by x4r's licence. Source: x4r/source/ and
https://github.com/countervolts/x4r
