# x4r (xess (xmx) 4 radeon)

want to run dlss on radeon? check out [d4r](https://github.com/countervolts/d4r)!

Experimental Intel XeSS XMX kernel translation for AMD GPUs under Wine and
vkd3d-proton. x4r translates Intel CM/GenX LLVM IR into wave-based HLSL, with
an int8 cooperative-matrix path for RDNA3. An igdext shim supplies the translated
kernels to the D3D12 application.

This is literally just another PoC, it's not really practical (yet atleast).

## Status

In Ghost of Tsushima through OptiScaler (856×480 input, 2560×1440 output, XeSS network
model KPSS) XeSS takes **about 2.05 ms of GPU time per frame** on the RX 7700 XT,
measured with GPU timestamps around every XeSS dispatch (stock dot4 kernels: about
4 ms). With steady GPU clocks the offline harness gives 2.13 ms at that setup and
2.22 ms at 1280×720 → 2560×1440; the kernels' speed does not depend on image content.
Performance measurements exclude rendering, presentation and pipeline creation.

Quality: on a static 3D fixture the WMMA, RDNA4-set and dot4 outputs match
byte-for-byte. On moving-camera fixtures (`x4r/tools/quality-motion.py`, with true
motion vectors and a moving object) they match the stock kernels to about 60 dB and
score 28.5–31 dB sRGB against a supersampled reference, within about 1 dB of XeSS's
own DP4a path; the moving object is where XeSS's XMX network is weakest, and a
mirror-image test (`quality-mirror.py`) shows no direction-dependent error in the
translation. In the game the picture is clean in motion. The XMX path has not been
shown to look *better* than DP4a: that needs real Intel XMX output as a reference.

XeSS selects its Intel extension/CM kernel path. x4r translates those kernels
to AMD instructions; matrix work runs on RDNA3 WMMA units. This is the
XMX-path network running through translation, with no Intel XMX hardware present.

## Install (release zip)

Drag and drop, OptiScaler style:

1. Extract the release zip into the folder holding the game's main `.exe`
   (it brings OptiScaler as `dxgi.dll`, XeSS 2.0.2.68 as `libxess.dll` and a
   `x4r/` folder). Use GE-Proton 11; no launch options.
2. Start the game and pick FSR 3.1 (or DLSS) in its graphics settings.
   OptiScaler replaces it with XeSS. The first launch sets x4r up by itself
   (shim into the Proton prefix, Intel vendor id in the game's `dxvk.conf`).
3. After playing a few seconds, run `bash x4r/x4r-check.sh`. It prints
   **PASS** only if the translated XMX kernels were dispatched, how many run on
   WMMA, and the XeSS GPU time.

The kernels are translated from XeSS **2.0.2.68**. x4r does not fall back
quietly: if a kernel cannot be built, XeSS fails and the check prints FAIL.
RDNA3 (gfx11) and RDNA4 (gfx12) each get their own WMMA kernel set, picked by
GPU; other AMD GPUs get the stock dot4 kernels. The RDNA4 set is untested on
RDNA4 hardware (it matches the RDNA3 output byte-for-byte on an RX 7700 XT).
Details: [packaging/README.txt](packaging/README.txt).

## Build the tools

The development environment uses Linux, LLVM **23.1.1**, a C++20-capable compiler
(the translator builds as C++17; the oracle uses C++20 barriers),
Vulkan headers/loader, Python 3 with NumPy and Pillow, and systemd user scopes.
The Windows quality harness additionally requires MinGW-w64. Shader builds use
the Linux DXC distribution; tested shaders are compiled at **`-O0`**.

```bash
./x4r/tools/setup-workspace.sh
./x4r/tools/build-tools.sh
```

The source repository does not contain Intel SDK binaries, captured kernels,
generated DXIL/SPIR-V, a Wine prefix, or prebuilt third-party tools. See
[development setup](docs/DEVELOPMENT.md) for the expected local dependencies and
[third-party notices](THIRD_PARTY.md) for their separate licences.

On a prepared runtime with baseline DXIL, build the selected fast shader profiles:

```bash
./x4r/tools/build-fast.sh
```

## License

x4r's own code is licensed under **GPL-3.0-only**; see [LICENSE](LICENSE).
Third-party components retain their original licences. This licence does not
grant rights to Intel's SDK, captured model/kernel material, or generated shader
artifacts. Review the applicable SDK terms before distributing derived kernels
or binaries; no such material is included in this source tree.
