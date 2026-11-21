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
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.20 | zlib; its bundled HIDAPI is used under HIDAPI's BSD-style license (HIDAPI is offered under GPL-3, a BSD-style or its original license, at the user's choice) | windows, the event loop and the clipboard in the `platform` client module; later keyboard, mouse, gamepads, text input, cursors and file dialogs | yes |
| [Slang](https://github.com/shader-slang/slang) | 2026.22.1 (prebuilt release) | Apache-2.0 WITH LLVM-exception | shader compilation in the `shader_compiler` tools module | no |
| [DXC](https://github.com/microsoft/DirectXShaderCompiler) | 1.9.2609 (prebuilt release) | Windows archive: `dxcompiler.dll` under the LLVM release license (University of Illinois/NCSA, `LICENSE-LLVM.txt`) and the `dxil.dll` validator under Microsoft's DirectX Shader Compiler license terms (`LICENSE-MS.txt`, which allows use on Windows only); the archive has no table mapping files to its three license files. Linux archive: its release notes put every file, `libdxcompiler.so` and `libdxil.so` included, under `LICENSE-LLVM.txt` (NCSA); it also contains the same `LICENSE-MS.txt` without naming files it covers | DXIL code generation, validation and signing, loaded by Slang | no |

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
