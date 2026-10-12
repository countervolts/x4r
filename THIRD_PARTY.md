# Third-party components

Dependencies are obtained separately and kept in the ignored local workspace.
The project licence applies to x4r-owned code, not these dependencies.

| Component | Source | Licence / handling |
|---|---|---|
| xess-xmx-linux / igdext shim | https://github.com/HHHHanasak1/xess-xmx-linux | MIT; preserve its copyright and licence when applying the local patch |
| LLVM | https://github.com/llvm/llvm-project | Apache-2.0 with LLVM exceptions |
| DirectXShaderCompiler | https://github.com/microsoft/DirectXShaderCompiler | Its distribution includes multiple licence notices; preserve them |
| vkd3d-proton | https://github.com/HansKristian-Work/vkd3d-proton | LGPL-2.1; consult the exact release's licence |
| DXVK | https://github.com/doitsujin/dxvk | zlib |
| Vulkan headers | https://github.com/KhronosGroup/Vulkan-Headers | Apache-2.0 / MIT notices in the headers |
| Intel XeSS SDK | https://github.com/intel/xess | Intel's SDK terms; not covered by GPL and not bundled |

The inspected XeSS SDK licence restricts reverse engineering and modification
of its binary software. The legality of redistributing captured kernels,
translated shaders or model data has not been established by this project.
Those artifacts, the SDK itself, and local research/captures stay outside the
source repository. The SDK's terms and third-party notices must be reviewed
independently of the licence on this repository's own code.

The shim patch targets upstream commit
`c8d481cf7a46b86645e2ffa12f665840b1665980`. Its changes do not replace the
upstream licence; retain the upstream MIT licence and the GPL notice on newly
added x4r code.

The upstream [xess-xmx-linux MIT notice](LICENSES/xess-xmx-linux-MIT.txt) and
[dxvk-igdext MIT notice](LICENSES/dxvk-igdext-MIT.txt) accompany the patch.
