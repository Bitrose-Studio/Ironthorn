# Third-party code

Every dependency is fetched at configure time as an archive pinned by URL and SHA-256
(`cmake/IronthornDependencies.cmake`). None is committed to the repository. Adding one needs:

- a license check (permissive licenses only in shipped code; no GPL/LGPL/CC BY-SA);
- a maintenance check;
- a platform check, including consoles;
- a line in the table below.

| Library | Version | License | Used for | Shipped in games |
|---|---|---|---|---|
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | unit tests | no |
| [yyjson](https://github.com/ibireme/yyjson) | 0.13.0 | MIT | JSON reading and writing for source data and config, private to the `serialization` module | yes |
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.20 | zlib; its bundled HIDAPI is used under HIDAPI's BSD-style license (HIDAPI is offered under GPL-3, a BSD-style or its original license, at the user's choice) | windows, the event loop and the clipboard in the `platform` client module; later keyboard, mouse, gamepads, text input, cursors and file dialogs | yes |
| [Slang](https://github.com/shader-slang/slang) | 2026.22.1 (prebuilt release) | Apache-2.0 WITH LLVM-exception | shader compilation in the `shader_compiler` tools module | no |
| [DXC](https://github.com/microsoft/DirectXShaderCompiler) | 1.9.2609 (prebuilt release) | Windows archive: `dxcompiler.dll` under the LLVM release license (University of Illinois/NCSA, `LICENSE-LLVM.txt`) and the `dxil.dll` validator under Microsoft's DirectX Shader Compiler license terms (`LICENSE-MS.txt`, which allows use on Windows only); the archive has no table mapping files to its three license files. Linux archive: its release notes put every file, `libdxcompiler.so` and `libdxil.so` included, under `LICENSE-LLVM.txt` (NCSA); it also contains the same `LICENSE-MS.txt` without naming files it covers | DXIL code generation, validation and signing, loaded by Slang | no |
| [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | 1.620.2 | MIT | Direct3D 12 API declarations and GUIDs in the `rhi_d3d12` client module (Windows) | yes (compiled in) |
| [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator) | 3.3.0 | MIT | GPU memory allocation in the `rhi_d3d12` client module (Windows) | yes |
| [DirectX 12 Agility SDK](https://aka.ms/directx12agility) (NuGet `Microsoft.Direct3D.D3D12`) | 1.620.2 | Microsoft DirectX license terms (`LICENSE.txt` in the package): the redistributables `D3D12Core.dll` and `d3d12SDKLayers.dll` may be shipped with an application, solely for use on Windows, under the terms' distribution requirements | the Direct3D 12 runtime that players and editors load on Windows | `D3D12Core.dll` yes; `d3d12SDKLayers.dll` no (not in Shipping builds) |

Slang and DXC are tools: only the `shader_compiler` module links them, and only when
`IRONTHORN_BUILD_TOOLS` is on (the default). Their release archives are downloaded at configure
time and their shared libraries are copied next to the executables that need them in the build
tree. Neither is committed, so the repository needs no notice for them. A distribution that
ships them (an editor, never a game) must include their license files, which are in the
archives.

SDL is built from its release archive as a static library, only when `IRONTHORN_BUILD_CLIENT` is
on (the default), and only the `platform` module links it; a dedicated server doesn't contain
it. The options (audio, rendering, GPU, camera, tray, haptics, sensors and OpenGL off; Vulkan,
dialogs, power, joystick and HIDAPI on) are in `cmake/IronthornDependencies.cmake`, and the
reasons in [ADR-0012](../decisions/0012-windowing-and-event-loop.md). zlib needs no notice in a
shipped game, but games ship SDL's and HIDAPI's license texts in their third-party notices
(`LICENSE.txt` and `src/hidapi/LICENSE-bsd.txt` in the archive).

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
