# x4r (xess (xmx) 4 radeon)

Experimental Intel XeSS XMX kernel translation for AMD GPUs under Wine and
vkd3d-proton. x4r translates Intel CM/GenX LLVM IR into wave-based HLSL, with
an int8 cooperative-matrix path for RDNA3. An igdext shim supplies the translated
kernels to the D3D12 application.

This is literally just another PoC, it's not really practical (yet atleast).

## Status

At **2560×1440 output and 1280×720 input**, the tuned shaders measured
**1.936, 1.932 and 1.934 ms of GPU upscale time per frame** on the RX 7700 XT.
The installed profile measured **1.119 ms at 1920×1080 output**.
These measurements use **Performance mode at 2×**, rather than the DLSS/FSR
Quality scale of 1.5×. Quality-scale optimisation and measurements are in progress.
The tuned output matches the preserved dot4/WMMA translation byte-for-byte on a
deterministic static 3D fixture. Motion and disocclusion quality remain untested.
Performance measurements exclude rendering, presentation and pipeline creation.

XeSS selects its Intel extension/CM kernel path. x4r translates those kernels
to AMD instructions; matrix work runs on RDNA3 WMMA units. This is the
XMX-path network running through translation, with no Intel XMX hardware present.

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
