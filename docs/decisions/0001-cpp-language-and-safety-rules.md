# ADR-0001: C++20 as the engine language, with safety rules

- **Status:** Accepted
- **Date:** 2026-11-20

## Context

The engine needs:

- console support within a few years;
- native access to graphics and game middleware (upscalers, GPU debuggers, physics, audio,
  navigation);
- a language that a small team can keep safe in a large, multithreaded codebase.

C++ and Rust were compared on middleware support, console toolchains, safety, iteration speed
and the pool of experienced developers.

## Decision

1. **Ironthorn is written in C++20.**
   - Every console platform's SDK, toolchain and certification process assumes C++.
   - The middleware is native, with no binding layers: DLSS/Streamline, FSR, XeSS, PIX, Jolt,
     FMOD/Wwise, Recast.
2. **Compilers.** MSVC and clang-cl on Windows, Clang on Linux, and GCC in CI as a second
   opinion. Clang is the reference compiler, because console toolchains are Clang-based.
3. **Language level.**
   - No C++20 modules until every target toolchain supports them reliably.
   - C++23 library features only behind engine wrappers. For example, `ith::Expected` mirrors
     `std::expected`.
4. **No exceptions** in any Ironthorn target, tests included, so the standard library is
   configured the same everywhere:
   - MSVC: `/EHs-c-` and `_HAS_EXCEPTIONS=0`.
   - Clang/GCC: `-fno-exceptions`.
   - Failures that can happen at run time return `ith::Expected<T, E>`.
   - Programmer errors are asserts: `ITH_ASSERT`, `ITH_VERIFY`, `ITH_FATAL`.
5. **No RTTI** in runtime modules (`ith::runtime_options`: `/GR-`, `-fno-rtti`). The engine
   gets its own generated reflection. Tests and tools may use RTTI.
6. **Ownership.**
   - No raw owning pointers.
   - Engine objects are referred to by generational handles.
   - Other owned objects use unique or reference-counted pointers.
   - Transient data uses arena and frame allocators.
   - Containers check access in Debug and development builds.
7. **Threads.**
   - Parallel work runs through the job system.
   - Shared state carries Clang thread-safety annotations (`-Wthread-safety` is on).
8. **Safety tooling** in CI:
   - Warnings are errors.
   - clang-tidy runs with `.clang-tidy`.
   - The Linux jobs run sanitizers: an **ASan + UBSan** job and a **TSan** job.
   - Parsers, serialization and network code get fuzz targets as they are written.
9. **Build.** CMake with presets and Ninja. Third-party code is pinned by URL and SHA-256
   (`cmake/IronthornDependencies.cmake`) and kept behind engine interfaces.
10. **API.**
    - Engine code lives in namespace `ith`, with headers under `engine/ith/<module>/`.
    - A stable C ABI is added for plugins and other languages when the first consumer needs it.
11. **Gameplay languages.**
    - C++ is first-class.
    - A visual graph runs on a bytecode VM (no JIT, so it is console-safe).
    - C# may come later as an optional plugin.

## Consequences

- Compile-time memory safety, which Rust would give, is not available. Rules 6–8 recover what
  is practical, and reviews enforce them.
- Code that relies on exceptions or RTTI can't be used in runtime modules, and third-party
  libraries are chosen with that in mind.
