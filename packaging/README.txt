x4r - Intel XeSS XMX path on AMD RDNA3 / RDNA4 (Linux, Proton)
===============================================================

x4r lets games that offer FSR 3.1 (or DLSS) use Intel XeSS's XMX network on a Radeon. OptiScaler (included) takes the
game's upscaler inputs and hands them to XeSS; x4r translates XeSS's Intel XMX kernels to AMD, where the matrix work runs
on RDNA's WMMA units. No Intel hardware is involved.

Install
-------
1. Extract everything in this zip into the folder that holds the game's main .exe (Unreal Engine:
   <game>/<Project>/Binaries/Win64). You get dxgi.dll, OptiScaler.ini, libxess.dll, Licenses/ and an x4r folder.
   If the folder already has a dxgi.dll or OptiScaler.ini (another OptiScaler install), move those out of the way
   first, and remove PROTON_USE_OPTISCALER from the game's launch options.
2. Use GE-Proton 11 for the game. No launch options are needed.
3. Start the game and choose FSR 3.1 (or DLSS) in its graphics settings. OptiScaler replaces it with XeSS.
   (Press Insert in the game for OptiScaler's menu.)
4. Optional check, from a terminal in the game folder after playing a few seconds:   bash x4r/x4r-check.sh
   It prints PASS only if the translated XMX kernels were dispatched, and how many run on WMMA.

The first launch sets x4r up by itself: the x4r plugin copies its shim into the game's Proton prefix (Proton fixes the
path XeSS loads it from; Proton's own file is kept as igdext64.dll.x4r-backup) and adds an x4r block to dxvk.conf in the
game folder that makes the GPU report an Intel vendor id (XeSS only enables its XMX path on Intel). Nothing else is
touched and no sudo is needed. Proton overwrites the shim when it updates a prefix; the next game launch puts it back.

Supported: Radeon RX 7000 and RX 9000 (RDNA3 gfx1100-1103, gfx1150/1151; RDNA4 gfx1200/1201) with Mesa RADV. Other AMD
GPUs get the stock dot4 kernels. x4r/x4r.ini can force a kernel set (Profile = auto | rdna3 | rdna4 | dxil).

Limits
------
* This build includes Ready or Not's HDR + automatic-exposure kernels AND its responsive-pixel-mask variant. The
  previous build missed the latter and still fell back to FSR. Kernel coverage was checked across 96 settings
  combinations; the matching responsive-mask setup renders successfully. Fully restart the game after updating:
  changing the upscaler within the existing process does not clear a cached missing-kernel failure.
* The kernels are for XeSS 2.0.2.68, the libxess.dll in this zip. If the game's own XeSS or another libxess.dll is used
  instead, XeSS fails rather than silently using DP4a (x4r-check.sh prints FAIL).
* Severe ghosting is unresolved on the RX 7700 XT, including with Profile = dxil. This build is not a ghosting fix.
  To compare with Intel's unmodified DP4a implementation, change Backend = dp4a in x4r/x4r.ini and restart the game.
  Change it back to Backend = x4r to use the translated XMX path. See X4R_GHOSTING_AUDIT.md in this archive.
* Tested locally: Radeon RX 7700 XT (gfx1101), synthetic fixtures and Ghost of Tsushima. Other games, other GPUs and
  the RDNA4 kernel set on RDNA4 hardware are not validated. This is not a universal installer. DX12 / Proton only;
  frame generation is not translated. Different XeSS flags or network models can require additional kernels.
* Research prototype, not affiliated with Intel, AMD or the OptiScaler project. Set IGDEXT_TRACE=1 in the launch options
  to get C:\igdext_trace.log inside the prefix; x4r/installation-status.txt reports the first-launch setup.

Uninstall: bash x4r/uninstall.sh, then delete dxgi.dll, OptiScaler.ini, libxess.dll, x4r and Licenses from
the game folder.

Licences: x4r is GPL-3.0-only. OptiScaler (dxgi.dll) is GPL-3.0; libxess.dll is Intel's XeSS SDK under the Intel Simplified
Software License (Licenses/XeSS_LICENSE.txt), redistributed unmodified. The shim derives from xess-xmx-linux and
dxvk-igdext (MIT, x4r/licenses). The translated kernels derive from XeSS and are not covered by x4r's licence.
Source: x4r/source and https://github.com/countervolts/x4r
