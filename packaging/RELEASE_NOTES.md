# x4r 0.1.0

First release: Intel XeSS's XMX network running on AMD RDNA3 and RDNA4 under Linux/Proton, by translating XeSS's Intel CM kernels to wave-based HLSL. The matrix work runs on RDNA WMMA units; no Intel hardware is involved.

## Install

1. Extract `x4r-0.1.0.zip` into the folder with the game's main `.exe` (Unreal Engine: `<Game>/Binaries/Win64`).
2. Launch the game once with Proton, close it, run `bash x4r/install.sh` (Steam games are found automatically; `--prefix DIR` otherwise).
3. Use GE-Proton 11, select XeSS in the DX12 renderer. No launch options.
4. Play a few seconds, run `bash x4r/x4r-check.sh`. **PASS** means the translated XMX kernels were dispatched.

`bash x4r/uninstall.sh` reverts it (Proton's `igdext64.dll` is kept as `.x4r-backup`).

## What's in it

- Drag-in release: patched `igdext64.dll` shim, 30 translated kernels, an installer that places the shim in the Proton prefix and adds a vendor-id block to `dxvk.conf`.
- Kernel sets for **RDNA3 (gfx11)** and **RDNA4 (gfx12)**, chosen automatically from the GPU; any other AMD GPU uses the stock dot4 kernels. `x4r.ini` can force a set.
- **No silent fallback:** if the game's XeSS is not 2.0.2.68, or a kernel cannot be built, XeSS pipeline creation fails and `x4r-check.sh` reports FAIL instead of quietly using DP4a or rendering noise.
- Proof of use: `x4r/status.txt` records GPU, kernel set, translated/WMMA/failed kernel counts, dispatch count and XeSS GPU time.

## Measured (RX 7700 XT, gfx1101, Performance mode 2x, synthetic fixture)

| Kernel set | XeSS GPU time / frame (quality harness) |
|---|---|
| WMMA (auto) | 3.25 ms |
| Stock dot4 | 4.93 ms |

Outputs of the WMMA, RDNA4-set and dot4 runs are byte-identical on the static fixture. Benchmark-harness timings at 1440p are in the README (1.93 ms).

## Limits

- Kernels are for XeSS **2.0.2.68** only.
- Tested on one GPU (RX 7700 XT) and a synthetic fixture. **The RDNA4 set has not run on RDNA4 hardware**, other RDNA3 GPUs and real games are untested, and motion/disocclusion quality is unverified. Please report results.
- Super resolution only; frame generation is not translated. Proton overwrites `igdext64.dll` on prefix updates: rerun `install.sh`.
- Research prototype, not affiliated with Intel or AMD. The translated kernels derive from Intel's XeSS and are not covered by x4r's GPL-3.0 licence; see THIRD_PARTY.md.

SHA-256: see `x4r-0.1.0.zip.sha256`.
