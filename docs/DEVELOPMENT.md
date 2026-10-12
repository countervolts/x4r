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
THIRD_PARTY.md) stages `igdext64.dll` + `x4r/` into `dist/x4r-<version>.zip`.
After editing the shim, regenerate `patches/igdext-x4r.patch`
(`git add -N src/dll/X4r.cpp && git diff --relative` in the shim); packaging
refuses a stale patch.
