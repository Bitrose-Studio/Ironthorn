# ADR-0011: Shader language and pipeline

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions, no RTTI),
  [ADR-0004](0004-modules-and-layering.md) (tools stay out of the runtime),
  [ADR-0005](0005-reference-hardware.md) (minimum features, optional features per tier),
  [ADR-0006](0006-math-and-coordinate-conventions.md) (matrix layout, clip space),
  [ADR-0009](0009-rhi.md) (bindless model, SPIR-V and DXIL input),
  [ADR-0010](0010-serialization.md) (binary archive, derived-data cache)

## Context

ADR-0009 fixes what the RHI accepts: SPIR-V for Vulkan, DXIL for Direct3D 12, every resource
reached by a bindless index, at most 128 bytes of constants. This ADR fixes where that bytecode
comes from. Constraints:

- **One shader source for both APIs.** Materials will later compile from a node graph, so the
  language needs modules and generics, not only text includes.
- **No shader-compile stutter in shipped games.** Every pipeline is known at cook time.
- **Fast iteration.** Editing a shader in the editor shows the result without a restart.
- **Variant explosion** is the usual way shader systems become slow to build and cook, so
  variants are budgeted from the start.
- The compiler is a tool. It never runs inside a shipped game, so it is not bound by the runtime
  rules of ADR-0001, but its API must be usable from code built with them.

Lessons from earlier engine prototypes by the same maintainer:

- Slang, compiled ahead of time to SPIR-V and DXIL behind a small compiler interface in a
  tools-only module, worked; about 5k lines of engine shaders were written in it. The editor and
  cooker linked the compiler; the player did not.
- Pinning the compiler by version and hash, and keying the shader cache on the source and every
  dependency, the compiler version, options and target, kept builds reproducible.
- Checking the compiled output against an allowlist of capabilities caught shaders that would
  have failed on devices without an optional feature.

Facts below were checked on 2026-11-20 against the projects' release pages and archives.

## Options considered

**A. Slang** ([v2026.22.1](https://github.com/shader-slang/slang/releases/tag/v2026.22.1),
2026-11-19; Apache-2.0 WITH LLVM-exception). One source compiled to SPIR-V and, through DXC, to
DXIL. Modules (`import`), generics and interfaces suit a material system. Reflection API, and
bindless helpers that map onto `ResourceDescriptorHeap` on D3D12 and descriptor-indexed arrays on
Vulkan. Prebuilt releases for Windows and Linux (x86-64 and ARM64); frequent releases (about
monthly), so upgrades need a deliberate pin.
- Its C++ API (`slang.h`, COM-style interfaces returning `SlangResult`) compiles with MSVC and
  clang-cl (Visual Studio 2022) using `/std:c++20 /EHs-c- /D_HAS_EXCEPTIONS=0 /GR- /W4 /WX`
  with no errors or warnings. The engine's `shader_compiler` module links it as a shared library,
  and its tests pass in CI on Windows (MSVC, clang-cl) and Linux (Clang, GCC). No exception
  crosses the interface; the library's internal use of exceptions stays inside its shared
  library.
- DXIL output needs DXC's `dxcompiler` and `dxil` libraries, which the Slang release does not
  include.

**B. HLSL with DXC for both APIs** (DXC
[v1.9.2609](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609),
2026-09-29). Mature, one compiler, native DXIL, SPIR-V via `-spirv`. No modules or generics
beyond what HLSL 2021 offers, so materials would be built by text generation, and the existing
shader code would be rewritten.

**C. A language per API** (HLSL for D3D12, GLSL for Vulkan). Two sources of every shader.
Rejected without further study.

For compiler integration, **ShaderMake** (NVIDIA, MIT, untagged) drives permutation builds for
DXC and Slang. It adds nothing over a small compiler module plus the cooker, so it isn't used.

## Decision

### Language and compiler

1. **Slang is the only shader language** for engine, material and user shaders. Shared code
   (conventions, BRDFs, bindless access) lives in engine Slang modules.
2. **Targets:** SPIR-V 1.6 for Vulkan 1.3, and DXIL Shader Model 6.6 for Direct3D 12. Stages that
   need more (mesh shaders, ray tracing) compile only in variants enabled for tiers whose devices
   report the feature (ADR-0005). No source forks per backend.
3. **Pins (tools only, never shipped in games):**

   | Tool | Version | Archive | SHA-256 |
   |---|---|---|---|
   | [Slang](https://github.com/shader-slang/slang) | 2026.22.1 | `slang-2026.22.1-windows-x86_64.zip` | `e72344f7b0323cec8b039c534caaf4680e7ba629489fc796ea4c7f7adb7deb68` |
   | | | `slang-2026.22.1-linux-x86_64-glibc-2.28.tar.gz` | `0c0e243ac15b7878237e6854313f45c791408227d5bc634f3ad1ac487edbfaf5` |
   | [DXC](https://github.com/microsoft/DirectXShaderCompiler) | 1.9.2609 | `dxc_2026_09_29.zip` | `ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1` |
   | | | `linux_dxc_2026_09_28.x86_x64.tar.gz` | `96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1` |

   - **Licenses.** Slang: Apache-2.0 WITH LLVM-exception. DXC, Windows archive: `dxcompiler`
     under the LLVM release license (NCSA) and the `dxil` validator under Microsoft's DirectX
     Shader Compiler license terms, which allow use on Windows only. DXC, Linux archive: its
     release notes put every file, `libdxil.so` included, under the NCSA license. Details are in
     `docs/development/third-party.md`.
   - The Linux Slang archive is the build without the LLVM-based CPU back end, which the engine
     doesn't use. Its glibc 2.28 name is not the toolchain minimum: DXC's Linux libraries need
     **glibc 2.38 or newer** (Ubuntu 24.04 or newer).

   - Each archive is pinned by URL and SHA-256 and listed in `docs/development/third-party.md`
     with the module that uses it. Tools are never taken from `PATH` or an SDK install.
   - Upgrading either is one reviewed change that passes the full shader test suite. Upgrades
     happen at phase boundaries, or sooner only for a needed fix or feature.
   - Linux CI also has DXC and compiles DXIL, so both targets are checked on both operating
     systems.
   - Editor distributions may ship Slang and DXC for user and material shaders, with their
     license notices. Games never ship them.
4. **The compiler is a tools-only module** (`shader_compiler`), linked by the editor and the
   cooker and never by the player. It uses Slang's library API and turns every failure into
   `ith::Expected` with the diagnostics attached. No Slang type leaves the module. If compiler
   crashes ever reach the editor, compilation moves to a `slangc` process behind the same
   module.
5. **Conventions:** column-major matrix layout and the engine clip space (ADR-0006). DXIL is
   validated and signed by DXC's validator. SPIR-V is validated by the glslang bundled with
   Slang (Slang's `ValidateWithDownstreamCompiler` option).

### Reflection and checks

6. **Reflection is checked, not trusted to define the layout.** After each compile the module
   reads Slang's reflection and stores engine-owned shader metadata next to the bytecode: entry
   points, stages, constant size and thread-group size. Compilation fails, naming the shader and
   entry point, when:
   - the shader declares a resource outside the bindless model of ADR-0009;
   - its constants exceed 128 bytes;
   - its output uses a capability outside the allowlist for its tier.

### Variants

7. **Variants are few and declared.** A shader lists its variant keys (each a small set of
   values). Anything that can be a runtime branch on a constant is a branch, not a key. A shader
   has **at most 32 variants**; CI fails a build that exceeds it. CI also reports the engine's
   total variant count, whose limit is set once the first renderer exists.

### Cache, cooking and shipping

8. **Shader cache keyed by content hash** of: the source and every file it imports (from Slang's
   dependency list), the Slang and DXC versions, the compile options, the target and the variant
   values. Entries live in the local derived-data cache and use the binary archive of ADR-0010.
   The cache becomes shared when the derived-data cache does.
9. **Shipped games use precompiled shaders only.** The cooker compiles every variant that the
   cooked content and tiers need into the package's shader library. A shipping build has no
   compiler and no source; a missing shader is a cook error and, at run time, an error that
   names the shader. Pipeline creation is driven by cooked data; pipeline caches and warm-up come
   with packaging.
10. **Hot reload in the editor and development builds only.** Changed sources are recompiled on a
    background job; on success the affected pipelines are replaced at a frame boundary, and on
    failure the old shader stays and the diagnostics go to the log.
11. **Debug information** is generated in development builds for GPU debuggers, and is stripped
    from shipping packages.

## Consequences

- One shader source for both APIs, with modules for materials, and no compiler in games.
- Two tool dependencies that release often; each upgrade is a deliberate, tested change.
- DXIL needs DXC beside Slang, on every machine that cooks for Direct3D 12; on Linux that means
  glibc 2.38 or newer.
- Variant budgets push work into runtime branches; some shaders will be slightly slower than a
  fully specialized build, in exchange for fast builds and small packages.
- Follow-up work: the reflection checks and cache in `shader_compiler`; the shader runtime
  (metadata, cache, library) with tests; the engine bindless Slang module; the capability allowlists per tier; the variant
  budget check in CI.
- **Revisit when:**
  - Slang stops supporting a target we need, or its release cadence breaks builds;
  - in-process compilation makes the editor unstable (move compilation to a `slangc` process);
  - a console target is chosen (its shader compiler joins behind the same module);
  - variant budgets can't be met without visible cost.
