# ADR-0023: Build-time shaders for samples and tests

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** amends [ADR-0011](0011-shader-language-and-pipeline.md) (rules 3, 4, 6 and 9);
  [ADR-0009](0009-rhi.md) (bindless model, SPIR-V and DXIL input),
  [ADR-0017](0017-runtime-and-editor-separation.md) (module kinds and targets)

## Context

ADR-0011 puts the shader compiler in the tools-only `shader_compiler` module (rule 4), keeps
Slang and DXC out of games (rule 3) and gives games precompiled, cooked shaders only (rule 9).
ADR-0017 lets a player link only `runtime` and `client` modules.

The first player sample, `scene_player`, and its GPU test draw with a few fixed Slang shaders.
Neither can use the intended path:

- the sample is a player target, so it can't link `shader_compiler`, and its test renders the
  same scenes library;
- the cooker, the asset pipeline and the derived-data cache that will deliver cooked shaders to a
  player don't exist yet.

The engine's RHI backend tests link `shader_compiler` and compile at run time; they are not
player targets and stay as they are.

The build already has the pinned Slang and DXC archives when `IRONTHORN_BUILD_TOOLS` is on. The
`ith_embed_shaders()` CMake helper runs the pinned `slangc` at build time and writes the bytecode
as C++ arrays into headers that the target's sources include: SPIR-V always, DXIL where the
Direct3D 12 backend is built. ADR-0011 doesn't say whether that is allowed.

**Prior art.** Unreal Engine separates *global shaders*, which work on fixed geometry and need no
material, from material shaders. Its public documentation says global shaders go through the same
shader compilation as the rest, a failed global shader compile is fatal, and in cooked builds they
are stored in a separate global shader file, loaded early at start-up, instead of inside material
packages ([Shader Development](https://dev.epicgames.com/documentation/unreal-engine/shader-development-in-unreal-engine),
[Adding Global Shaders](https://dev.epicgames.com/documentation/unreal-engine/adding-global-shaders-to-unreal-engine),
both checked 2026-11-21). Built-in shaders there are cooked data with one compile pipeline, not
a second build-time path. How that file is generated and invalidated isn't described in the pages
read.

## Options considered

- **Build-time `slangc` for samples and tests only** (chosen). Works today with the pins already
  fetched; no tool runs in the player; the bytecode is the same as `shader_compiler` would
  produce if both use the same options. Risk: a second compile path that can drift from the
  module, so the rules below bind it to the module.
- **A tools program that links `shader_compiler` and writes the headers.** One compile path by
  construction, and later reflection checks apply for free. Costs a new executable and build step
  now, for a handful of shaders, before the module has anything (reflection checks) that
  `slangc` lacks. Kept as the fallback in rule 4.
- **Wait for the cooker.** No player sample and no GPU scene test until the asset pipeline
  exists. Rejected: the RHI needs an end-to-end player now.
- **Commit precompiled bytecode.** No build-time tool, but binaries in the repository go stale
  against their sources and pins without anyone noticing. Rejected.
- **Also cover engine built-in shaders** (full-screen passes, blits, debug drawing). Not needed
  yet: no renderer exists. When it does, built-in shaders should follow Unreal's model and ADR-0011
  rule 9, compiled by `shader_compiler` and cooked into the shader library. Left out.

## Decision

1. **Build-time compilation is allowed only for shaders embedded in samples and engine tests**,
   through `ith_embed_shaders()`. The targets that use it are built only with
   `IRONTHORN_BUILD_TOOLS` on. Games, engine modules and engine built-in shaders don't use it;
   they follow ADR-0011 rule 9.
2. **The pinned tools only.** It runs the `slangc` of the ADR-0011 rule 3 Slang pin and passes the
   pinned DXC explicitly (`-dxc-path`), never a tool from `PATH` or an SDK. Rule 3's "tools only,
   never shipped" still holds: only the bytecode is embedded.
3. **Options identical to `shader_compiler`.** One documented list, in
   `docs/development/shaders.md`, names the options both paths use: targets and profiles
   (SPIR-V 1.6, DXIL SM 6.6), column-major matrices, SPIR-V validation through the downstream
   compiler, entry-point names kept in SPIR-V, and the `ITH_SPIRV` define for SPIR-V. A comment
   at the option code in `ShaderCompiler.cpp` and in `ith_embed_shaders()` points to that list.
   A change to one side changes the list and the other side in the same commit. `shader_compiler`
   adopts `ITH_SPIRV` (it doesn't define it today), until the engine's bindless Slang module
   replaces the define.
4. **Reflection checks cover embedded shaders too.** When `shader_compiler` gains the checks of
   ADR-0011 rule 6, embedded shaders must pass them as well: either the same checks run in the
   build-time path, or build-time compilation moves behind the module (a small tools program that
   links it). Duplicating the checks in CMake is not allowed.
5. **Correct rebuilds.** Each generated header depends on its source, on `slangc` and, for DXIL,
   on the DXC libraries, and takes Slang's depfile so imported files trigger a rebuild. A
   compile error fails the build.

## Consequences

- `scene_player` and its GPU test build and run now, on both APIs, with the pinned compilers and
  no tools code in the player.
- Samples using embedded shaders need `IRONTHORN_BUILD_TOOLS`, so the Shipping link job of
  ADR-0017 rule 4 (tools off) doesn't cover them until they load cooked shaders.
- A second compile path exists, kept in step by one list and review; drift shows as different
  bytecode or as tests passing on one path only.
- Follow-up work: `docs/development/shaders.md` with the option list; the cross-reference
  comments; `ITH_SPIRV` in `shader_compiler`.
- ADR-0011 is marked as amended by this ADR when this ADR is accepted.
- **Revisit when:**
  - the cooker and derived-data cache deliver shaders to a player (samples switch to cooked
    shaders; decide whether tests keep this path);
  - `shader_compiler` gains reflection checks (rule 4);
  - the renderer needs built-in shaders (they go through `shader_compiler` and the cooker, not
    this path, unless a new ADR says otherwise).
