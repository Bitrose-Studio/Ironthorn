# Architecture overview

This page describes what Ironthorn is meant to be and how it is put together. The decisions
behind it are recorded as [ADRs](decisions/README.md). The page describes the target; what
exists today is listed under [Roadmap](#roadmap).

## Goals

1. **3D first, 2D supported.** Both share one ECS, one asset pipeline, one editor, UI, audio and
   input. 2D is an optional module.
2. **Modular.** A small core is always present. Engine modules and plugins are opt-in per
   project, and a shipped game links only what it uses.
3. **High-end image quality:**
   - physically based materials and dynamic global illumination;
   - high-quality shadows, sky, atmosphere, clouds and volumetrics;
   - temporal upscaling, GPU particles, terrain and foliage.
4. **Graphics that scale without breaking the look.** See [Scalability](#scalability).
5. **Game UI outside the world.** Widgets are assets owned by the player and the HUD, never
   objects in the scene hierarchy. See [Game UI](#game-ui).
6. **Production gameplay foundations:**
   - server-authoritative networking with client prediction;
   - an abilities, attributes and effects system;
   - AI, save games and open-world streaming.
7. **Engineering discipline:**
   - tests for everything;
   - CI on Windows and Linux;
   - decisions recorded as ADRs;
   - the simplest design that does the job ([coding standards](development/coding-standards.md)).

Not goals for now: mobile, web, film or architectural visualization, and replacing modelling
tools. Consoles are planned after the first PC release, and no design decision may rule them out.

## Layers

```text
 application   editor · player (game runtime) · dedicated server · tools
 editor        editor modules: viewport, outliner, details, content browser, graph editors ·
               tools-only modules (shader compiler)
 framework     gameplay framework · abilities/attributes/effects · AI · save · gameplay kit
 engine        renderer (3D, 2D) · UI · physics · animation · audio · net · navigation · VFX ·
               terrain · foliage · sky and weather · streaming · scripting
 core          ECS world · scheduler · reflection · serialization · assets · input · events
 platform      window · event loop · file system · threads · jobs · RHI and its backends
 foundation    memory · containers · math · strings · asserts · Expected · logging · profiling
```

Rules ([ADR-0004](decisions/0004-modules-and-layering.md)):

- A module depends only on modules of its own or a lower layer, without cycles. A test checks
  this.
- The runtime never depends on the editor.
- The renderer, audio, physics and UI don't own the scene. They read data extracted from it
  each frame.
- No third-party or graphics-backend types appear in public module headers.

Foundation conventions: math types and coordinates follow
[ADR-0006](decisions/0006-math-and-coordinate-conventions.md), and logging and profiling follow
[ADR-0007](decisions/0007-logging-and-profiling.md).

## Modules today

| Module | Layer | What it is for | What it must not do |
|---|---|---|---|
| [`foundation`](../engine/ith/foundation) | foundation | Asserts (`ITH_ASSERT`, `ITH_VERIFY`, `ITH_FATAL`) with a replaceable handler; `ith::Expected<T, E>` for failures without exceptions; the engine version; string helpers; `Stopwatch` and `FixedTimestep`; FNV-1a hashing; `ith::Uuid`; the `LinearAllocator` arena and `ObjectPool<T>`; compiler and sanitizer hooks (`Compiler.h`) | Depend on anything but the C++ standard library; hold engine state |
| [`reflection`](../engine/ith/reflection) | core | Generated reflection ([ADR-0018](decisions/0018-reflection.md), [guide](development/reflection.md)): the `ITH_TYPE`, `ITH_FIELD` and `ITH_ENUM` annotations; `TypeInfo` and `FieldInfo` records with stable FNV-1a ids, layout, value functions (construct, copy, move, destroy, field-wise equality), flags and attributes; `ith::typeOf<T>()` with hand-written leaf types and `std::array`, `std::vector`, `std::optional`; `TypeRegistry` with lookup by id, name and `FormerName`. Modules opt in with `REFLECT` in `ith_add_module` | Use RTTI or static initializers; register types after startup; hold editor-only data in runtime types |
| [`shader_compiler`](../engine/ith/shader_compiler) | editor (tools only) | `ith::ShaderCompiler`: compiles a self-contained Slang source for one entry point and stage to SPIR-V 1.6 or to signed DXIL Shader Model 6.6, through Slang's library API and DXC, with the compiler diagnostics as the error ([ADR-0011](decisions/0011-shader-language-and-pipeline.md)). Built only with `IRONTHORN_BUILD_TOOLS` | Be linked by a game or any runtime module; expose Slang types |

Build tools in `tools/` run on the host during the build and are linked into nothing.
`ith_reflect` scans the annotated headers of each module that sets `REFLECT` and writes its
`<module>.reflect.cpp`; it needs no third-party code and is built in every configuration.

Test support (`tests/support`) provides the doctest entry point and `ith::test::AssertCapture`,
which records failed asserts so tests can check them.

## World model

- The **ECS** stores entities in archetype tables, with relations, deferred commands, change
  detection and a parallel scheduler.
- The editor shows the ECS as an ordered hierarchy of entities with components in the Outliner.
  "Actor" is an authoring word for an entity with a transform. See [Editor](#editor).
- **Levels** are assets, and a **world** instantiates them. Open worlds stream in cells.
- **Prefabs** nest, and changes to a prefab reach the copies that haven't overridden them.
- The gameplay framework provides game mode, game state, player controller, player state,
  pawn/character, camera manager, and subsystems scoped to the engine, world, game instance and
  local player.
- Gameplay tags and data assets are first-class.

## Editor

The editor follows Unreal Engine workflow conventions, so an Unreal developer can work in it
without retraining ([ADR-0008](decisions/0008-editor-familiar-to-unreal-developers.md)):

- The default layout puts the viewport in the centre with a toolbar above it (Play and
  Simulate), the Outliner and Details panels at the side, and the Content Browser below, also
  available as a content drawer. Panels dock, and layouts are saved.
- It uses the same generic vocabulary: Level, Actor, Component, Outliner, Details, Content
  Browser, Play In Editor, Simulate, Possess and Eject, World Settings, Project Settings and
  Editor Preferences.
- Viewport shortcuts default to an Unreal-style preset, and every binding can be remapped.
- The Details panel is generated from reflection.
- The Content Browser mirrors the folders on disk. Assets have stable ids, so moving or renaming
  one needs no redirector.
- Configuration is layered: engine, then project, then platform, then user.

Only workflow conventions are followed. No Unreal Engine code, icons, styles or product names
are used.

## Rendering

- A render graph schedules passes, aliases memory and places barriers.
- Rendering is bindless and GPU-driven, with a visibility buffer or deferred G-buffer.
- Materials are node graphs compiled to Slang. Shader pipelines are compiled ahead of time, so
  games don't stutter.
- Backends: Vulkan 1.3 and Direct3D 12, behind an engine-owned RHI. Mesh shaders and hardware
  ray tracing are optional and enabled per tier
  ([ADR-0005](decisions/0005-reference-hardware.md)).
- Space is right-handed with +Y up and −Z forward. Scene depth is reversed, and rendering is
  camera-relative ([ADR-0006](decisions/0006-math-and-coordinate-conventions.md)).

## Scalability

Low graphics settings in many games make the picture worse but barely faster. Ironthorn treats
this as a design problem:

1. **Scale precision, not presence.** Look-defining features have a ladder of implementations:
   - global illumination, shadows, ambient occlusion;
   - atmosphere and fog;
   - material response.

   Lower tiers reduce resolution, sample counts or update rates. The lowest rung is a cheap
   approximation tuned to match the top one, not removed.
2. **Bake-down.** What high tiers compute live is baked for low tiers at cook time: probe
   lighting, reflections, far-field shadows and impostors. The lowest tier lights the scene the
   same way, only less dynamically.
3. **Resolution first.** Internal resolution plus temporal upscaling saves the most time for the
   least visible loss.
4. **Amortize over time.** Expensive updates run at tiered rates and are reprojected in between.
5. **Fix the real bottleneck.** The runtime tells whether a frame is CPU-, GPU- or memory-bound.
   It only turns the settings that relieve that bottleneck.
6. **No popping.** Continuous level of detail replaces hard draw-distance cuts.
7. **Measured, not guessed.**
   - Each setting registers its cost.
   - A benchmark tool measures time, memory and perceptual error (FLIP) per setting on
     reference hardware.
   - [ADR-0005](decisions/0005-reference-hardware.md) sets the reference hardware and targets
     per tier. Low is a GTX 1650 4 GB desktop at 60 fps and the Steam Deck at 30 fps (40 fps
     as an option). Epic is 4K at 60 fps. The benchmark lab is phased in as machines join it.
   - Presets are built from the best trade-offs.
   - CI fails when a lower preset no longer saves enough time.

## Game UI

- **Widget assets** are edited in a designer. They hold a widget hierarchy, layout slots,
  styles, bindings and event handlers.
- Widgets live in the viewport's UI layer, owned by the HUD and the player controller, never in
  the scene hierarchy. A widget shown in the world is a component that *references* a widget
  asset.
- **View models** with change notification feed widgets. Nothing polls every frame.
- **Layers and activatable widgets** cover the HUD, menus, modals and notifications. They handle
  focus, gamepad navigation and back actions.
- An input router consumes input, so an open menu blocks gameplay input.
- Text is shaped with HarfBuzz and drawn as MSDF. The UI renderer is shared with 2D.

## Languages

- The engine is written in C++20 ([ADR-0001](decisions/0001-cpp-language-and-safety-rules.md)).
- Gameplay code is C++, plus a visual graph that runs on a bytecode VM.
- C# may come later as an optional plugin, through a stable C ABI.

## Roadmap

Each phase ends when its exit criteria pass in CI.

| Phase | Content | State |
|---|---|---|
| P0 | Repository, build, CI, foundation module, first ADRs | **in progress** |
| P1 | Platform, jobs, RHI (Vulkan, D3D12), render graph, reflection, serialization, assets, input | planned |
| P2 | Editor shell following Unreal Engine workflow conventions ([ADR-0008](decisions/0008-editor-familiar-to-unreal-developers.md)): viewport, outliner, details, content browser, dockable saved layouts, remappable input with the Unreal-style default preset, undo, play-in-editor, prefabs | planned |
| P3 | Renderer v1 and the scalability framework | planned |
| P4 | Game UI framework and designer | planned |
| P5 | Gameplay core: physics, animation, audio, scripting, abilities, networking, dedicated server | planned |
| P6 | Gameplay kit: inventory, interaction, equipment, crafting, quests, dialogue, save | planned |
| P7 | Open world: streaming, terrain, foliage, water, sky and weather, navigation, AI | planned |
| P8 | Renderer v2: volumetrics, GPU particles, probe GI with bake-down, virtual shadow maps | planned |
| P9 | Production hardening: packaging, crash reporting, localization, accessibility, profiler | planned |
| P10 | 2D toolset | planned |

Version 0.1.0 is released when P1 is complete
([ADR-0002](decisions/0002-git-workflow-and-versioning.md)).
