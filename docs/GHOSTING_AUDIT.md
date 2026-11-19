# Ghosting investigation — 2026-10-12

The ghosting is unresolved. The new Desktop package is a consistent build of the
current runtime, not a verified image-quality fix.

## Follow-up: Ready or Not compatibility fix

The first 48-kernel update was insufficient: the subsequent game run requested
`66fbdd75308e78e4`, the HDR/auto-exposure responsive-pixel-mask variant (flags
266). Its output-filter companion is `bdf80aabadc6e0fa`. Both are now included
as `dyn_75` and `dyn_103`; the latter has WMMA overrides.

To cover further setting changes, 96 combinations were captured: HDR/LDR,
scalar/texture/automatic exposure, ordinary/inverted depth, low/high-resolution
motion, jittered/nonjittered motion, and responsive mask on/off. They requested
130 unique kernels, all hash-verified, translated and compiled; none is missing
from the package. Capture only created placeholders without dispatching them.
The package has 23 overrides per GPU generation and otherwise uses translated
DXIL. A real-pipeline initialization sweep passed the first 16 combinations
before being stopped to prioritize rendering the failing setup. This does not
establish that all 96 combinations were rendered or initialized with real PSOs.
The flags-266 rendering test uses a nonzero responsive mask (0.5), not a null
pointer or inactive mask.

Fully exit and restart Ready or Not after updating: missing bytecode is cached
for the lifetime of the game process. Selecting XeSS again within the old
process cannot repair a cached failure.

The following describes the earlier HDR-only update and its validation:

Ready or Not's installed status reported four pipeline failures for missing
kernel `d09865d3c47fe34c`, with only two translated pipelines created. That
explains its XeSS-to-FSR fallback. The HDR + automatic-exposure setup (init
flags 258: inverted depth, automatic exposure) was reproduced in an isolated
Wine prefix with initialization only; placeholder kernels were never dispatched.
It requires two kernels absent from the old package:

- `dyn_46`, `d09865d3c47fe34c`: input processing.
- `dyn_47`, `e104d3c5dc73f2f3`: output filtering/network.

Both were captured, hash-verified, lifted and compiled. The second also has
RDNA3 and RDNA4 WMMA overrides. The package now includes 48 baseline kernels
and 22 overrides per GPU generation. The matching HDR/auto-exposure fixture
initializes and renders with zero failed pipelines in DXIL and WMMA. The two
frame-19 output readbacks are byte-identical, and the driver log confirms the
new output-filter override loaded. All windowed CPU-oracle checks pass for
the new output-filter WMMA profile. The input-processing check encounters the
half-rounding boundary described below, so it is not an exact oracle pass.
Game installation
is separate from a user-confirmed in-game result; the user will restart and test.

Actual anti-ghosting experiments were rejected: reconstruction-based history
rejection reduced the trail but lost 5.29 dB whole-image quality; RGB neighborhood
clipping lost 2.41 dB in motion and 6.37 dB on the static fixture. They are not
in the shipped shaders. Disabling E-forms/forwarding in both shared input and
output shaders produced byte-identical motion output. Switching the 3x fixture
from legacy Performance to Ultra Performance also produced identical output.

A CPU/GPU oracle mismatch at a half conversion was investigated rather than
treated as an automatic fix: the preceding float differs by two FP32 ULPs,
straddling a half-rounding midpoint. The original cast and an explicit integer
RNE conversion agree on 327,680 finite/edge test cases on the GPU; changing the
conversion did not remove the oracle mismatch and was not shipped. Native
floating-point approximation remains an evidence limit, not a confirmed cause
of the severe ghosting.

The user reports camera-wide ghosting through both native XeSS and FSR inputs,
including with `Profile = dxil`; DP4a does not show the same problem.
That narrows the investigation to the shared XMX translation/temporal pipeline.
It does not identify a particular faulty instruction or prove Intel's XMX model
itself is responsible.

## Evidence checked

The existing local moving-object fixture has correct object and camera motion
vectors, 856×480 input, 2560×1440 output, KPSS model, init flags 70, and an
explicit pipeline build. Recalculation of the frame-19 readbacks gives:

| Path | Whole image, sRGB PSNR | Vacated-object region, sRGB PSNR |
| --- | ---: | ---: |
| Intel DP4a | 29.37 dB | 25.34 dB |
| Translated XMX, DXIL | 28.44 dB | 22.68 dB |
| Translated XMX, WMMA | 28.45 dB | 22.66 dB |
| Translated XMX, DXIL + full UAV barriers | 28.44 dB | 22.68 dB |

Higher is better. The vacated region contains 63,522 pixels: locations occupied
by the red sphere on the first frame, excluding a 12-pixel expansion of its
final position. XMX retains substantially more red there. Whole-image averages
hide this concentrated trail. A fresh run using the current compiled shim and
current baseline DXIL reproduced the earlier DXIL capture byte-for-byte, with
finite output and 32 translated pipelines, zero failed pipelines.

The barrier and ordinary DXIL captures are byte-identical. This rules out the
tested inter-dispatch UAV ordering hypothesis for this fixture. It does not
verify every game's external resource transitions. The existing WMMA and DXIL
captures differ by 58.95 dB linear RGB; they exhibit the same trail. The full
image comparison is not a proof that every translated operation is correct.

These are local synthetic readbacks, not a new game capture or native Intel XMX
comparison. The moving-object test does not fully reproduce the reported
camera-wide symptom.

## Where to investigate next

1. **Shared motion/history processing and translation.** The `input_processing`
   family (`dyn_2`, `32`, `34`, `38`, `40`, `42`, `43`, plus newer variants) reads
   motion and samples history. Both WMMA and stock DXIL originate in
   `x4r/src/x4rwave.cpp`. Its 3D lowering around line 1413 emits texture loads and
   `SampleLevel(smp0, uv, 0)`; float/half conversion and signed arithmetic also
   affect history coordinates, validity and network features. Compare those
   intermediate outputs against native Intel XMX on the exact same inputs.
   Agreement with the CPU oracle alone cannot rule out shared assumptions in
   the oracle and translator. DP4a uses a different implementation/network and
   is a useful quality reference, not an instruction-level XMX reference.
2. **Actual execute inputs and init flags.** Check motion scale, jitter inclusion
   and sign, low/high-resolution motion, inverted depth, exposure, input
   subrects, and history reset in the failing game. Camera-wide trails fit
   wrong reprojection or history rejection. Both frontend routes failing makes
   an FSR-only conversion bug less likely. The synthetic test reproduces an
   XMX quality gap with correct inputs, but does not clear the game's inputs.
3. **Kernel/root-signature compatibility.** The runtime selects DXIL by the CM
   bytecode/compile-options hash; it does not validate the runtime root signature
   against the one used for translation. The translator also assumes one
   static sampler and handles only the first descriptor range per parameter,
   dropping register-space information. The captured signatures use the
   expected simple bindings, so there is no demonstrated mismatch here. This
   remains a compatibility risk for an “any game” claim.

The 46 captured kernels currently use only sample opcode 0 / load opcode 26
with zero immediate offsets (90 sample calls and 148 loads). The translator
does not implement arbitrary sampler opcodes/offsets, but that omission is not
an observed explanation for this captured set. The older sampler-binding and
Proton override-initialization problems are already addressed in this checkout;
they cannot be assumed to explain ghosting that persists in stock DXIL.

Intel's input requirements and troubleshooting guidance:
https://github.com/intel/xess/blob/main/doc/xess_sr_developer_guide_english.md

## Build and install

The shim was rebuilt with CMake/MinGW, one job. Two captured kernels absent from
the previous release (`dyn_44` and `dyn_45`) were compiled at `dxc -O0`, bringing
the baseline set to 46 in the first package. The Ready or Not follow-up adds
two more and one override per GPU generation, as described above. The shim patch
was regenerated from the current local source and the packager checked that it
reproduces `X4r.cpp` and `D3D12Api.cpp`. Existing edits to `quality.cpp` and
`x4rsim.cpp` were preserved.

Extract the Desktop ZIP beside the game's main executable. It includes
OptiScaler as `dxgi.dll`, matching XeSS 2.0.2.68, the plugin, kernel sets,
configuration, checks and uninstall script. Use GE-Proton 11 and DX12, select
FSR 3.1 / DLSS for OptiScaler replacement, then run `bash x4r/x4r-check.sh`.
PASS means dispatch was observed; it does not certify correct image quality or
that vkd3d actually loaded every override.

For an A/B comparison, restart after changing `x4r/x4r.ini`:

```ini
Backend = x4r
Profile = auto
```

Use `Profile = dxil` for translated XMX without WMMA overrides. Use
`Backend = dp4a` for Intel's original DP4a implementation. `Trace = true`
enables kernel traces. Do not enable `Capture` for normal play: unmatched
kernels use placeholders during capture and output can be wrong.

This supports the supplied DX12/Proton setup, not every game/API/flag combination.
Different settings can require additional translations. A failed missing-kernel
build is distinct from ghosting produced by successfully created pipelines.
