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
