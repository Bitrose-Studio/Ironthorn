# Third-party code

Every dependency is fetched at configure time as an archive pinned by URL and SHA-256
(`cmake/IronthornDependencies.cmake`). None is committed to the repository. Adding one needs:

- a license check (permissive licenses only in shipped code; no GPL/LGPL/CC BY-SA). Fonts are
  the one exception: SIL OFL-1.1 is allowed for fonts only
  ([ADR-0029](../decisions/0029-editor-ui-toolkit.md));
- a maintenance check;
- a platform check, including consoles;
- a line in the table below.

| Library | Version | License | Used for | Shipped in games |
|---|---|---|---|---|
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | unit tests | no |
| [yyjson](https://github.com/ibireme/yyjson) | 0.13.0 | MIT | JSON reading and writing for source data and config, private to the `serialization` module | yes |
| [glm](https://github.com/g-truc/glm) | 1.0.3 | MIT (offered as "Happy Bunny" or MIT; used under MIT) | vectors, matrices and quaternions behind the `ith/math` aliases ([ADR-0006](../decisions/0006-math-and-coordinate-conventions.md)), header-only | yes (compiled in) |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.4 | BSD-2-Clause (the library header; the archive's `xxhsum` tool, GPL-2.0, is not built) | XXH3-128 content hashes for derived-data cache keys, private to the `asset_database` tools module (`Hash128.cpp`) | no |
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.20 | zlib; its bundled HIDAPI is used under HIDAPI's BSD-style license (HIDAPI is offered under GPL-3, a BSD-style or its original license, at the user's choice) | windows, the event loop and the clipboard in the `platform` client module; later keyboard, mouse, gamepads, text input, cursors and file dialogs | yes |
| [Slang](https://github.com/shader-slang/slang) | 2026.22.1 (prebuilt release) | Apache-2.0 WITH LLVM-exception | shader compilation in the `shader_compiler` tools module, and its `slangc` at build time for the shaders samples embed (`ith_embed_shaders()`) | no |
| [DXC](https://github.com/microsoft/DirectXShaderCompiler) | 1.9.2609 (prebuilt release) | Windows archive: `dxcompiler.dll` under the LLVM release license (University of Illinois/NCSA, `LICENSE-LLVM.txt`) and the `dxil.dll` validator under Microsoft's DirectX Shader Compiler license terms (`LICENSE-MS.txt`, which allows use on Windows only); the archive has no table mapping files to its three license files. Linux archive: its release notes put every file, `libdxcompiler.so` and `libdxil.so` included, under `LICENSE-LLVM.txt` (NCSA); it also contains the same `LICENSE-MS.txt` without naming files it covers | DXIL code generation, validation and signing, loaded by Slang | no |
| [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | 1.620.2 | MIT | Direct3D 12 API declarations and GUIDs in the `rhi_d3d12` client module (Windows) | yes (compiled in) |
| [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator) | 3.3.0 | MIT | GPU memory allocation in the `rhi_d3d12` client module (Windows) | yes |
| [DirectX 12 Agility SDK](https://aka.ms/directx12agility) (NuGet `Microsoft.Direct3D.D3D12`) | 1.620.2 | Microsoft DirectX license terms (`LICENSE.txt` in the package): the redistributables `D3D12Core.dll` and `d3d12SDKLayers.dll` may be shipped with an application, solely for use on Windows, under the terms' distribution requirements | the Direct3D 12 runtime that players and editors load on Windows | `D3D12Core.dll` yes; `d3d12SDKLayers.dll` no (not in Shipping builds) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.4.363 (SDK 1.4.363.0) | Apache-2.0 OR MIT | Vulkan API declarations (C API only) in the `rhi_vulkan` client module | yes (compiled in) |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 3.5.0 | MIT | GPU memory allocation in the `rhi_vulkan` client module | yes |

Slang and DXC are tools: only the `shader_compiler` module links them, and the build runs
`slangc` with them for embedded sample shaders, only when `IRONTHORN_BUILD_TOOLS` is on (the
default). Their release archives are downloaded at configure
time and their shared libraries are copied next to the executables that need them in the build
tree. Neither is committed, so the repository needs no notice for them. A distribution that
ships them (an editor, never a game) must include their license files, which are in the
archives.

xxHash is header-only: `asset_database`'s `Hash128.cpp` is the only file that includes
`xxhash.h`, with `XXH_INLINE_ALL`, so no library is built and no xxHash symbol is visible outside
that file. The archive's command-line tool (`cli/`, GPL-2.0) is never compiled. Editors and tools
that ship it include its BSD-2 license text (`LICENSE` in the archive).

SDL is built from its release archive as a static library, only when `IRONTHORN_BUILD_CLIENT` is
on (the default), and only the `platform` module links it; a dedicated server doesn't contain
it. The options (audio, rendering, GPU, camera, tray, haptics, sensors and OpenGL off; Vulkan,
dialogs, power, joystick and HIDAPI on) are in `cmake/IronthornDependencies.cmake`, and the
reasons in [ADR-0012](../decisions/0012-windowing-and-event-loop.md). zlib needs no notice in a
shipped game, but games ship SDL's and HIDAPI's license texts in their third-party notices
(`LICENSE.txt` and `src/hidapi/LICENSE-bsd.txt` in the archive).

glm is header-only and its own CMake project is not used: FetchContent only unpacks the archive,
and the `ith::glm` INTERFACE target carries its include directory (as a system include) and the
ADR-0006 configuration defines as interface definitions, so no translation unit sees glm without
them. Only the `math` module links it, and only files in `engine/ith/math` include it (the
`glm_includes` test). Its `test/` and `doc/` directories are never built. Games ship its MIT
license text (`copying.txt` in the archive) in their third-party notices. A future dependency
that includes glm itself must be checked for a conflicting configuration ([ADR-0006](../decisions/0006-math-and-coordinate-conventions.md)).

yyjson is built from its release archive as a static library and linked only by the
`serialization` module, whose `Json.cpp` is the only file that includes it
([ADR-0010](../decisions/0010-serialization.md)). File I/O and the incremental reader are turned
off and its reader and writer are limited to 64 levels of nesting
(`cmake/IronthornDependencies.cmake`). Games ship its MIT license text (`LICENSE` in the
archive) in their third-party notices.

The Direct3D 12 dependencies are fetched only on Windows with `IRONTHORN_BUILD_CLIENT` on, and
only the `rhi_d3d12` module links them. DirectX-Headers and D3D12MA are built from their release
archives; D3D12MA's one source file is compiled by the engine's build, against DirectX-Headers and
without exceptions. Games ship their MIT license texts in their third-party notices.

The Agility SDK package is downloaded at configure time and never committed. Only two of its
files are used: `ith_use_d3d12_agility_sdk()` copies `D3D12Core.dll`, and outside Release
(Shipping) `d3d12SDKLayers.dll`, into a `D3D12` folder next to each player, editor and Direct3D
12 test executable. The package's `distributable files.txt` names both as distributable. Its
license is not an open-source license: it allows use "solely for use on Windows", and
distribution only with significant added functionality, under terms that protect Microsoft at
least as much as the license, and with an indemnity. **The redistribution terms must be checked
before the first release** (ADR-0009). Its version always matches DirectX-Headers.

The Vulkan dependencies are fetched with `IRONTHORN_BUILD_CLIENT` on, and only the `rhi_vulkan`
module uses them. Only Vulkan-Headers' C API is used (`vulkan_core.h` and the window-system
headers), without Vulkan-Hpp. VMA is one header, whose implementation `rhi_vulkan` compiles in one
of its own source files, without exceptions or RTTI. No Vulkan library is linked: `rhi_vulkan`
opens the system's Vulkan loader at run time. Games ship both MIT license texts (Vulkan-Headers
is also offered under Apache-2.0) in their third-party notices.

## Test tooling (not built, not shipped)

These are installed on developer machines and CI runners, not fetched by CMake. The GPU tests
use them when they are present.

| Tool | Version | License | Used for | Shipped in games |
|---|---|---|---|---|
| [Mesa lavapipe](https://docs.mesa3d.org/drivers/llvmpipe.html) (Ubuntu `mesa-vulkan-drivers`) | the ubuntu-24.04 package (25.2.8 at the time of writing) | MIT (uses LLVM, Apache-2.0 WITH LLVM-exception) | CPU Vulkan driver for the `gpu`-labelled tests on Linux CI | no |
| [Khronos validation layer](https://github.com/KhronosGroup/Vulkan-ValidationLayers) (from the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) Linux tarball) | 1.4.363.1, pinned by SHA-256 in CI | Apache-2.0 | API and synchronization validation in Vulkan tests (CI and developer machines) | no |
| Vulkan loader (system: Ubuntu `libvulkan1`, driver-installed `vulkan-1.dll`) | system | Apache-2.0 | loaded at run time by `rhi_vulkan`; never linked or redistributed | no (the player's system provides it) |
