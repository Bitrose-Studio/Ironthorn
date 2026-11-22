# Building and testing

## Windows

Requirements:

- Visual Studio 2022 with the **Desktop development with C++** workload.
- The **C++ Clang tools for Windows** component (clang-cl, for the `windows-clang` preset).
- The Windows optional feature **Graphics Tools**, for the Direct3D 12 debug layer that Debug
  builds and the `rhi_d3d12` tests turn on ([RHI guide](rhi.md#the-agility-sdk)). Without it, run
  the tests with `ITH_RHI_TEST_VALIDATION=off`.

```powershell
./scripts/bootstrap.ps1          # once: pinned CMake, Ninja, clang-format and clang-tidy into .tools/
./scripts/dev.ps1 build          # MSVC, Development configuration (RelWithDebInfo)
./scripts/dev.ps1 test
./scripts/dev.ps1 verify         # the pre-commit gate (see below)
```

Useful options:

- `-Config Debug|RelWithDebInfo|Release`
- `-Preset windows|windows-clang`
- `-Target <name>`
- `-Filter <ctest regex>`

`dev.ps1` loads the MSVC environment by itself.

### Pinned tools

`bootstrap.ps1` downloads these into `.tools/`, each pinned by URL and SHA-256. Run it again
after pulling a change to the pins; it skips tools that are already at the pinned version.

| Tool | Version | Source |
|---|---|---|
| CMake | 4.4.4 | Kitware release archive |
| Ninja | 1.13.2 | ninja-build release archive |
| clang-format | 23.1.4 | `clang-format` wheel on PyPI (a zip; no Python needed) |
| clang-tidy | 23.1.4 | `clang-tidy` wheel on PyPI, with Clang's builtin headers |

`dev.ps1 format`, `format-check` and `tidy` use only the pinned clang-format and clang-tidy,
never the copies that come with Visual Studio. Their versions differ between machines and CI,
and a different version formats some code differently and runs different checks. The compiler
is not pinned: clang-cl comes from Visual Studio, because it has to match Visual Studio's
standard library. The pinned clang-tidy reads the `windows-clang` build's
`compile_commands.json` and parses the code with its own, newer front end.

To upgrade clang-format and clang-tidy, change both to the same version in `bootstrap.ps1`
(hashes from `scripts/hash-url.ps1`), run `dev.ps1 format` and `dev.ps1 tidy`, and commit the
reformatting separately from the tool change.

## Linux

Requirements: Clang 18+ (or GCC 13+), CMake 3.28+, Ninja, and, for the client modules (on by
default), the display development packages below.

SDL, which the `platform` module builds from source, compiles in its X11 and Wayland video
drivers, IME, gamepad hotplug, HIDAPI's libusb backend and file dialog support only when
their development headers are found at configure time; without them it silently builds without
that support. On Ubuntu 24.04 or Debian:

```sh
sudo apt-get install pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
    libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libegl-dev \
    libdecor-0-dev libdbus-1-dev libibus-1.0-dev libudev-dev libusb-1.0-0-dev
```

The `platform` test fails when the `x11` or `wayland` driver is missing. If you installed the
packages after a first configure, delete the build directory (or at least `CMakeCache.txt` and
`_deps/sdl3-build`) so SDL checks again. A build with `-DIRONTHORN_BUILD_CLIENT=OFF` (a dedicated
server) needs none of them.

```sh
cmake --preset linux-clang
cmake --build --preset linux-clang-dev
ctest --preset linux-clang-dev
```

The `rhi_vulkan` tests (label `gpu`) need a Vulkan driver and the loader; without them they
report themselves skipped. Mesa's lavapipe runs them on the CPU:

```sh
sudo apt-get install mesa-vulkan-drivers libvulkan1 vulkan-tools
```

The validation layer, which the tests turn on with synchronization validation in Debug builds,
comes from the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) (1.4.363.1, as in CI);
Ubuntu 24.04's `vulkan-validationlayers` is too old. Environment variables:

- `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json` uses only lavapipe, as CI does;
- `VK_ADD_LAYER_PATH=<sdk>/share/vulkan/explicit_layer.d` finds the SDK's validation layer;
- `ITH_RHI_TEST_ADAPTER=software` (or `hardware`) picks the kind of device;
- `ITH_RHI_TEST_REQUIRE_VULKAN=1` makes a missing device or layer a failure instead of a skip.

Sanitizer builds:

```sh
cmake --preset linux-asan && cmake --build --preset linux-asan && ctest --preset linux-asan
cmake --preset linux-tsan && cmake --build --preset linux-tsan && ctest --preset linux-tsan
```

Sanitizer builds (`IRONTHORN_ENABLE_SANITIZERS` or `IRONTHORN_ENABLE_TSAN`) build the tools
without the shader compiler: CMake sets the internal `ITH_BUILD_SHADER_COMPILER` off and says so.
Slang and DXC are prebuilt, uninstrumented shared libraries, so a sanitizer would check only the
`shader_compiler` module's thin wrapper around them; the other Linux and Windows builds test it.
With it go its tests, the shaders that tests and samples embed at build time and `scene_player`.
The other `tools` modules, such as `asset_database`, are built and checked.

The reflection generator `ith_reflect` is instrumented too and runs during the build, so the
sanitizer runtime's needs apply at build time, not only to `ctest`. Clang 18's runtimes fail on
kernels with high ASLR entropy; lower it before configuring and building, as CI does:
`sudo sysctl vm.mmap_rnd_bits=28`. Clang sanitizer builds also build `ith_reflect_fuzz`, a
libFuzzer target for the generator, and run it briefly as the `reflect_tool_fuzz` test, and
`ith_serialization_text_fuzz` and `ith_serialization_binary_fuzz` for the serialization readers
(the `serialization_text_fuzz` and `serialization_binary_fuzz` tests), and `ith_rhi_vulkan_spirv_fuzz` for
the Vulkan backend's SPIR-V check (the `rhi_vulkan_spirv_fuzz` test), and `ith_asset_sidecar_fuzz`
for the asset database's sidecar and header readers (the `asset_sidecar_fuzz` test), and `ith_gltf_import_fuzz`
for the glTF importer (the `gltf_import_fuzz` test). Clang ASan builds compile
all code with libFuzzer's coverage instrumentation (`-fsanitize=fuzzer-no-link`), so the fuzz
targets get coverage feedback from the libraries they call; other executables don't link libFuzzer.

The ASan + UBSan builds turn off UBSan's `vptr` check (`-fno-sanitize=vptr`). It needs the type
information of every polymorphic class it checks, which engine modules, built without RTTI, never
emit; in tests, which keep RTTI, it would make classes such as an importer fail to link.

The `linux-tsan` test preset sets `TSAN_OPTIONS` with `tests/tsan-suppressions.txt`, which holds
the reports TSan makes about system libraries the engine can't change, each with its reason.
Run `ctest --preset linux-tsan`, or pass the same `TSAN_OPTIONS` when starting a test by hand.

Shipping link test ([ADR-0017](../decisions/0017-runtime-and-editor-separation.md)): the sample
players and server built in Shipping without the modules they must not link, as CI does. The
player preset needs the display packages above; the server preset doesn't:

```sh
cmake --preset linux-shipping-player && cmake --build --preset linux-shipping-player
cmake --preset linux-shipping-server && cmake --build --preset linux-shipping-server
```

## Module kinds and build options

Every module has a kind besides its layer
([ADR-0017](../decisions/0017-runtime-and-editor-separation.md)), declared with
`ith_add_module(<name> LAYER <layer> KIND <kind> …)`; the default is `runtime`.

| Kind | Holds | Layers | May depend on | Built when |
|---|---|---|---|---|
| `runtime` | code every game process needs | `foundation` to `framework` | `runtime` | always |
| `client` | code a dedicated server doesn't run | `foundation` to `framework` | `runtime`, `client` | `IRONTHORN_BUILD_CLIENT` |
| `tools` | offline processing, never in a game | `editor` | `runtime`, `tools` | `IRONTHORN_BUILD_TOOLS` |
| `editor` | the editor | `editor` | any kind | `IRONTHORN_BUILD_EDITOR` |

The three options are on by default. A module whose kind is off is not declared at all, so
anything that links it fails to configure. The editor needs the client and tools modules:
when either is off, CMake turns `IRONTHORN_BUILD_EDITOR` off too and says so.

Executables are applications, declared with
`ith_add_application(<name> KIND <target> SOURCES … MODULES …)`:

| Target | May link kinds | Configurations | Skipped when |
|---|---|---|---|
| `editor` | all | Debug, Development | `IRONTHORN_BUILD_EDITOR` is off |
| `player` | `runtime`, `client` | Debug, Development, Shipping | `IRONTHORN_BUILD_CLIENT` is off |
| `server` | `runtime` | Debug, Development, Shipping | never |
| `tools` | `runtime`, `tools` | Debug, Development | `IRONTHORN_BUILD_TOOLS` is off |

The `module_layering` test checks the whole dependency closure of each application against
its row. Editor and tools applications are left out of the default build of `Release`
(Shipping): the `all` target of Ninja Multi-Config, the solution build of Visual Studio.
Building one by name in `Release` is not refused, but nothing ships it, and a test that runs one
must not run in `Release`. Only applications are excluded: `tools` and `editor` modules and their
tests still build and run in `Release`, so `verify` covers them in every configuration. The
samples `minimal_player`, `window_player`, `scene_player` (built only with the shader compiler,
whose `slangc` compiles its shaders), `minimal_server` (`samples/`) and the editor,
`ironthorn_editor` (`editor/`, built with the shader compiler), are the applications today.

Sanitizer builds leave out the shader compiler. The `editor` modules don't need it, so they and their
headless tests (`editor_core`, `editor_ui`) are built and run under ASan, UBSan and TSan too
([ADR-0029](../decisions/0029-editor-ui-toolkit.md) rule 19): `editor_ui` carries its shader as
source and its tests render on the null device, which takes any bytecode. The editor application,
which compiles the UI shader at start-up, needs the shader compiler and is left out there.
`scene_player` and its GPU test need the shader compiler's `slangc`, so their code
(`samples/scene_player/Scenes.cpp`) gets no sanitizer coverage.

With the editor modules on, the build copies the editor fonts and their licences to `fonts/` next
to the executables of each configuration, as it copies `engine/config` to `Config/`
([editor UI guide](editor-ui.md#theme-fonts-and-icons)).

Each application is meant to call the registration function of every module it links, from a
source file generated out of its module list (ADR-0017 rule 5). That is deferred until the first
module registration function exists ([ADR-0016](../decisions/0016-gameplay-model-on-the-ecs.md));
the samples register nothing today.

## Tools and their dependencies

`IRONTHORN_BUILD_TOOLS` (on by default) builds the `tools` modules, which the editor and the
cooker link and games never do. Today these are `shader_compiler` and `asset_database`. The
shader compiler's prebuilt Slang and DXC
releases (Windows and Linux, x86-64) are downloaded at configure time, pinned by SHA-256. Their
shared libraries are copied next to the test executable after each link, so `ctest` runs
without any setup. Configure with `-DIRONTHORN_BUILD_TOOLS=OFF` to build without the tools
modules and these downloads. The build also runs Slang's `slangc` to compile the shaders that
samples and their tests embed (`ith_embed_shaders()`), so those exist only with the tools.

On Linux the tools need glibc 2.38 or newer (Ubuntu 24.04 or newer), which DXC's libraries
require, and zlib (`libz.so.1`).

The build tools in `tools/` are different: the build itself runs them, so they are built in
every configuration whatever `IRONTHORN_BUILD_TOOLS` says. Today that is `ith_reflect`, the
reflection generator ([reflection](reflection.md)), which uses only the standard library and
`foundation`.

## Configurations

| Configuration | Meaning | Asserts |
|---|---|---|
| `Debug` | unoptimized, full checks | on |
| `RelWithDebInfo` | **Development**: optimized, the everyday configuration | on |
| `Release` | **Shipping**: what players get | off |

## The pre-commit gate

`./scripts/dev.ps1 verify` runs the following and prints one PASS/FAIL line per step:

- format check
- MSVC builds and tests in Debug, Development and Release
- a clang-cl build and tests
- clang-tidy

Run it before every commit ([ADR-0002](../decisions/0002-git-workflow-and-versioning.md)). CI
runs the same gate on Windows, with the same pinned tools, plus Linux Clang, GCC, ASan + UBSan
and TSan builds, and the shipping link test in a plain Ubuntu container, where only the player
leg gets the display packages. Formatting and clang-tidy run only in the Windows job.

## Tests

- Each suite is an executable `ith_<name>_tests` registered with CTest as `<name>`, using
  doctest.
- Lint checks also run as CTest tests (label `lint`), among them:
  - `module_layering`: module layering rules ([ADR-0004](../decisions/0004-modules-and-layering.md))
    and the kind and application target rules
    ([ADR-0017](../decisions/0017-runtime-and-editor-separation.md)). The
    `module_layering_refuses_*` tests run the same check on made-up module tables and pass when
    it reports the expected violation.
  - `changelog_fragments`: changelog fragment format.
  - `imgui_confined`: Dear ImGui only in the `editor_ui` module, its internals only in
    `DockLayout.cpp` ([ADR-0029](../decisions/0029-editor-ui-toolkit.md) rule 2).
- Each sample application runs once as `sample_<name>` (label `sample`). `window_player` runs
  for 10 frames on SDL's headless `offscreen` video driver. So does `scene_player`, which
  renders nothing there; the `scene_player` GPU suite draws its scenes.
- The editor runs as `editor_automation_null` and `editor_automation_gpu` (labels `sample`, and
  `gpu` for the second): `ironthorn_editor` on the `offscreen` driver runs
  `tests/editor/open_edit_save_reopen.json` in a fresh project and exits 0, on the null device and
  on the software GPU. Not in `Release`, where the editor isn't built.
- `editor_ui_gpu` (label `gpu`) compares a golden screenshot of the editor shell on WARP and
  lavapipe, stored as binary PPM with Git LFS ([editor UI guide](editor-ui.md#tests)).
- Tests that need a GPU carry the `gpu` label. Skip them with `-ExcludeLabel gpu`
  (`ctest -LE gpu` on Linux). On Windows they run the Direct3D 12 backend on WARP, which every
  Windows machine has, so verify and CI run them. The Vulkan tests run on the machine's GPU, or
  report themselves skipped where no Vulkan driver is installed ([RHI guide](rhi.md#testing-on-a-gpu)).
