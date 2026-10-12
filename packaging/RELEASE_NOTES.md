# x4r 0.1.0

First release: Intel XeSS's XMX network running on AMD RDNA3 and RDNA4 under Linux/Proton. x4r translates XeSS's Intel CM kernels to wave-based HLSL; the matrix work runs on RDNA WMMA units. No Intel hardware is involved.

## Install

1. Extract `x4r-0.1.0.zip` into the folder with the game's main `.exe` (Unreal Engine: `<Game>/Binaries/Win64`). If the folder already has a `dxgi.dll`, `OptiScaler.ini` or `libxess.dll`, move them away first; the zip's `libxess.dll` (XeSS 2.0.2.68) replaces the game's.
2. Use GE-Proton 11 for the game. No launch options, no installer to run.
3. Start the game and pick FSR 3.1 (or DLSS) in its graphics settings. OptiScaler replaces it with XeSS.
4. Optional check after playing a few seconds: `bash x4r/x4r-check.sh`. **PASS** means the translated XMX kernels were dispatched.

On first launch the bundled plugin copies its shim into the game's Proton prefix and adds an Intel vendor-id block to the game's `dxvk.conf` (XeSS only enables its XMX path on Intel). `x4r/uninstall.sh` reverts both.

## What's in it

- OptiScaler 0.9.4 (`dxgi.dll`, GPL-3.0, unmodified) preconfigured for XeSS, plus Intel's XeSS 2.0.2.68 `libxess.dll` (unmodified, Intel Simplified Software License included).
- x4r plugin (`x4r/x4r.asi`) with 44 translated XeSS kernels, covering the pipelines OptiScaler's XeSS setup requests in Ghost of Tsushima (network model KPSS, LDR color, exposure texture, inverted depth, low-resolution motion vectors).
- Kernel sets for **RDNA3 (gfx11)** and **RDNA4 (gfx12)**, 21 WMMA/mixed-dot overrides each, chosen automatically from the GPU; other AMD GPUs use the stock dot4 kernels. `x4r/x4r.ini` can force a set (`Profile`), or run XeSS's own DP4a path under the same setup for comparison (`Backend = dp4a`).
- **No silent fallback:** if the XeSS in use is not 2.0.2.68 or a kernel cannot be built, XeSS pipeline creation fails and `x4r-check.sh` reports FAIL instead of quietly using DP4a or rendering noise.
- Proof of use: `x4r/status.txt` records GPU, kernel set, translated/WMMA/failed kernel counts, dispatch count and XeSS GPU time. `Trace = true` in `x4r.ini` adds per-kernel GPU times.

## Measured (RX 7700 XT, gfx1101)

| Setup | WMMA set | Stock dot4 kernels |
|---|---|---|
| Ghost of Tsushima, 856×480 → 2560×1440, in game | **about 2.05 ms** | about 4 ms |
| Same setup, offline harness, steady GPU clocks | 2.13 ms | 3.93 ms |
| 1280×720 → 2560×1440, offline harness | 2.22 ms | 4.00 ms |

Offline, the WMMA set, the RDNA4 set and the stock kernels give identical output on a static fixture and agree to about 60 dB on moving-camera fixtures with real motion vectors (28.5–31 dB sRGB against a supersampled reference, within about 1 dB of XeSS's own DP4a path). The XMX path is not shown to look better than DP4a; that needs real Intel XMX output as a reference. The XMX network is weakest on independently moving objects with poor motion vectors.

## Limits

- Kernels are for XeSS **2.0.2.68** only, and for the settings OptiScaler's default setup requests; other XeSS settings may need kernels that are not translated yet (XeSS then fails visibly). `Capture = true` in `x4r.ini` records the missing ones.
- Tested on one GPU (RX 7700 XT) with synthetic fixtures and Ghost of Tsushima. **The RDNA4 set has not run on RDNA4 hardware**; other RDNA3 GPUs and other games are untested. Please report results.
- dyn_32's float reciprocal differs from the CPU oracle by 1 ulp; dyn_24 runs on the stock path because its WMMA build disagrees with the oracle.
- Super resolution only; frame generation is not translated. Proton overwrites the shim on prefix updates: the next launch restores it.
- Research prototype, not affiliated with Intel, AMD or the OptiScaler project. The translated kernels derive from Intel's XeSS and are not covered by x4r's GPL-3.0 licence; see THIRD_PARTY.md.

SHA-256: see `x4r-0.1.0.zip.sha256`.
