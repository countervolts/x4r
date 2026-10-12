# Development setup

The checkout contains project source and tools. Runtime dependencies and
captured inputs are local development data and are not supplied with it.

`x4r/tools/setup-workspace.sh` creates ignored `private/` directories and legacy
compatibility symlinks at the checkout root. Tool scripts share portable paths
through `x4r/tools/common.sh`. Set `X4R_ROOT` or `X4R_SCRATCH` to override them.

Expected local paths:

| Path | Contents |
|---|---|
| `private/deps/dl-dxc` | Linux DXC distribution (`bin/dxc`, `lib`, `include/hlsl`) |
| `private/vendor/xess-sdk` | Separately obtained XeSS SDK headers and libraries |
| `private/third_party/xess-xmx-linux` | Upstream shim checkout plus local patch |
| `private/kernels` | Local IR, kernel map and root-signature map |
| `private/prefix` | Local Wine prefix and application/runtime DLLs |
| `private/quality`, `private/shots` | Measurement and capture output |
| `private/generated/build` | Project tools, scratch shaders and optional dxil-spirv tool (`x4r/build` symlink) |
| `private/generated/out` | Generated shader source (`x4r/out` symlink) |

For the shim:

```bash
git clone https://github.com/HHHHanasak1/xess-xmx-linux.git private/third_party/xess-xmx-linux
git -C private/third_party/xess-xmx-linux checkout c8d481cf7a46b86645e2ffa12f665840b1665980
git -C private/third_party/xess-xmx-linux/shim apply ../../../../patches/igdext-x4r.patch
```

The patch includes the x4r shim source. Build its DLL using the upstream
MinGW/CMake instructions, with a single build job under `safe.sh`. Local runtime
paths inside the Wine prefix are `C:\x4r` and `C:\x4r_run`.

Build captured shaders one at a time at `dxc -O0`. Rebuilding a kernel's DXIL
changes its override filename hash, so rebuild the matching WMMA override too.
The baseline WMMA override set is:

```bash
x4r/tools/build-wmma.sh 12 7 25 27 28 26 29 13 18 23
```

For the validated fast profiles, use `x4r/tools/build-fast.sh` instead. It
rebuilds dyn_2 DXIL and eleven SPIR-V overrides. Dyn_2 uses native mixed-sign
integer dot products; the ten matrix kernels use cooperative matrices. The
per-kernel packing, operand cache and aligned LDS settings are in
`x4r/tools/fast-profile.sh`. Rebuild the translator before using this recipe.

The split dispatch metadata must match the override set. Do not enable an
override with mismatched `splitz.txt` metadata. Isolated experimental builds can
use `X4R_TRANSLATOR` and `wmma-candidate.sh` without replacing the normal tools.

For 1440p measurement on a prepared local runtime:

```bash
free -m
X4R_OVR=1 EXE=sample1440.exe SECS=45 x4r/tools/safe.sh x4r/tools/gputime1440.sh
```

CPU oracle checks and GPU captures answer different questions. Passing the
oracle verifies translated operations on its inputs; a static scene capture
does not establish motion quality or game compatibility.

The 1440p timing tool defaults to the fixed-resolution `sample1440.exe` and
rejects dispatch grids that differ from the 2560×1440/1280×720 fixture. Desktop
window size is only presentation; it does not establish XeSS texture dimensions.

For matching the DLSS/FSR Quality scale (1.5× per dimension), use the headless
benchmark. It selects XeSS Legacy Quality, queries the exact input dimensions,
and measures GPU timestamps around the complete XeSS execute span after warmup:

```bash
x4r/tools/bench-upscale.sh build
x4r/tools/bench-upscale.sh run quality 2560 1440
```

The wrapper runs init-only first and rejects dummy/untranslated CM pipelines
before dispatch. Its output texture is fixed by the command-line dimensions;
no desktop window resize enters the benchmark. Input is a procedural static
pattern, so timings are harness measurements rather than game performance or
image-quality validation. `performance` selects the earlier legacy 2× scale.

## Kernel sets and release zip

`build-fast.sh` builds two WMMA override sets from the same translator: `ovr`
(RDNA3, gfx11) and `ovr-rdna4`. They differ only in dyn_29: the RDNA3 set keeps
the accumulator in registers (`X4R_WMMA_REGC`), which relies on RADV's gfx11
wave32 layout, while the RDNA4 set stages it through LDS using only
layouts the Vulkan spec defines. `X4R_RDNA=4` selects that in `fast-profile.sh`.
The portable set was run on gfx1101 and matches the RDNA3 output byte-for-byte;
it has not run on gfx12 hardware.

At run time the shim (`X4r.cpp`) reads the GPU name Wine takes from RADV
(`HKLM\...\Enum\PCI`), sets `VKD3D_SHADER_OVERRIDE` to the matching set before
the first kernel is built, and writes `x4r/status.txt`.
`x4r/tools/package-release.sh` (needs `X4R_REDISTRIBUTE_KERNELS=1`, see
THIRD_PARTY.md, and `X4R_OPTISCALER=<OptiScaler 0.9.x archive>`) stages the
OptiScaler bundle, `x4r.asi` (the same DLL as the shim; loaded as a
plugin it installs itself into the prefix) and `x4r/` into
`dist/x4r-<version>.zip`.

New XeSS kernels (another XeSS setup, e.g. OptiScaler's): run once with
`X4R_CAPTURE=1` so the shim records them instead of failing, then
`x4r/tools/lift-kernel.sh <ids>`, add the ids to `kernels/map.txt` and
`kernel_rootsig.txt`, `build-kernel.sh <id>`, and check with `tracecheck.sh`.
After editing the shim, regenerate `patches/igdext-x4r.patch`
(`git add -N src/dll/X4r.cpp && git diff --relative` in the shim); packaging
refuses a stale patch.

## Things learned the hard way

- **The SPIR-V overrides must bind the static sampler like vkd3d does** (descriptor set 3,
  binding 0; `bindless.py` does it). With dxc's default (set 0) the override of any kernel
  that samples a texture reads the wrong descriptor, which is invisible with zero motion
  vectors (samples land on texel centres) and wrong otherwise. Compare the SPIR-V vkd3d
  generates (`VKD3D_SHADER_DUMP_PATH`) with the override when an override disagrees with
  the DXIL.
- **`VKD3D_SHADER_OVERRIDE` has to exist when vkd3d-proton initialises.** Setting it from
  the shim (after the device exists) works under plain Wine but not under Proton, where the
  overrides silently did nothing and the split-kernel dispatch scaling then ran stock
  kernels with a wrong grid. The plugin (`x4r.asi`) now sets it as soon as it loads.
  Confirm with `Profile = dxil` vs `auto` and `Trace = true`: per-kernel times must differ.
- **Test with motion.** The static fixture hid both bugs above. Fixtures: `quality-motion.py`
  (camera pan/orbit, optional moving object with or without motion vectors, input
  resolution via `X4R_IW`/`X4R_IH`), `quality-mirror.py`, harness env `XESS_FLAGS`,
  `XESS_BUILD` (explicit `xessD3D12BuildPipelines`, as OptiScaler does: needed to request
  the same kernels as the game), `X4R_FRAMES`/`X4R_REPEAT` (timing with steady GPU clocks:
  one execute per submission leaves the clocks ramping and gives noisy numbers).
- **WMMA ideas that did not pay off:** batching independent dpas (`X4R_WMMA_BATCH`, correct
  but slower: register use drops occupancy), `dxc -O3` for overrides (no change), and a
  WMMA build of dyn_24, which disagrees with the CPU oracle (zeros at its first dpas).
  Option sweeps for dyn_41/7/26/40 found the current profiles already optimal.
- **A WMMA override needs the oracle too.** `tracecheck.sh <id>` with the profile's
  environment exported checks the exact build that ships.
- dyn_41, dyn_39, dyn_35, dyn_33 are the network kernel of the other XeSS setups; dyn_40,
  38, 34, 32, 42, 43 the history/first-layer family; they share dyn_29's and dyn_2's profiles.
