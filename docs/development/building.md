# Building and testing

## Windows

Requirements:

- Visual Studio 2022 with the **Desktop development with C++** workload.
- The **C++ Clang tools for Windows** component (clang-cl, clang-format, clang-tidy).

```powershell
./scripts/bootstrap.ps1          # once: pinned CMake and Ninja into .tools/
./scripts/dev.ps1 build          # MSVC, development configuration (RelWithDebInfo)
./scripts/dev.ps1 test
./scripts/dev.ps1 verify         # the pre-commit gate (see below)
```

Useful options:

- `-Config Debug|RelWithDebInfo|Release`
- `-Preset windows|windows-clang`
- `-Target <name>`
- `-Filter <ctest regex>`

`dev.ps1` loads the MSVC environment by itself.

## Linux

Requirements: Clang 18+ (or GCC 13+), CMake 3.28+, Ninja.

```sh
cmake --preset linux-clang
cmake --build --preset linux-clang-dev
ctest --preset linux-clang-dev
```

Sanitizer builds:

```sh
cmake --preset linux-asan && cmake --build --preset linux-asan && ctest --preset linux-asan
cmake --preset linux-tsan && cmake --build --preset linux-tsan && ctest --preset linux-tsan
```

Sanitizer builds (`IRONTHORN_ENABLE_SANITIZERS` or `IRONTHORN_ENABLE_TSAN`) always build without
the tools: CMake turns `IRONTHORN_BUILD_TOOLS` off and says so. The shader compiler's Slang and
DXC are prebuilt, uninstrumented shared libraries, so a sanitizer would check only the module's
thin wrapper around them; the other Linux and Windows builds test it.

The reflection generator `ith_reflect` is instrumented too and runs during the build, so the
sanitizer runtime's needs apply at build time, not only to `ctest`. Clang 18's runtimes fail on
kernels with high ASLR entropy; lower it before configuring and building, as CI does:
`sudo sysctl vm.mmap_rnd_bits=28`. Clang sanitizer builds also build `ith_reflect_fuzz`, a
libFuzzer target for the generator, and run it briefly as the `reflect_tool_fuzz` test.

## Tools and their dependencies

`IRONTHORN_BUILD_TOOLS` (on by default) builds the tools-only modules, which the editor and the
cooker link and games never do. Today that is `shader_compiler`. Its prebuilt Slang and DXC
releases (Windows and Linux, x86-64) are downloaded at configure time, pinned by SHA-256. Their
shared libraries are copied next to the test executable after each link, so `ctest` runs
without any setup. Configure with `-DIRONTHORN_BUILD_TOOLS=OFF` to build only the runtime
modules without these downloads.

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
| `RelWithDebInfo` | **development**: optimized, the everyday configuration | on |
| `Release` | **shipping**: what players get | off |

## The pre-commit gate

`./scripts/dev.ps1 verify` runs the following and prints one PASS/FAIL line per step:

- format check
- MSVC builds and tests in Debug, development and Release
- a clang-cl build and tests
- clang-tidy

Run it before every commit ([ADR-0002](../decisions/0002-git-workflow-and-versioning.md)). CI
runs the same gate on Windows, plus Linux Clang, GCC, ASan + UBSan and TSan builds.

## Tests

- Each suite is an executable `ith_<name>_tests` registered with CTest as `<name>`, using
  doctest.
- Two lint checks also run as CTest tests:
  - `module_layering`: module layering rules ([ADR-0004](../decisions/0004-modules-and-layering.md)).
  - `changelog_fragments`: changelog fragment format.
- Tests that need a GPU carry the `gpu` label. Skip them with `-ExcludeLabel gpu`
  (`ctest -LE gpu` on Linux).
