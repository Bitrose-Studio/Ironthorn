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
- Every module also has a **kind** ([ADR-0017](decisions/0017-runtime-and-editor-separation.md)):
  `runtime` (every game process), `client` (not on a dedicated server), `tools` (offline
  processing) or `editor`. `runtime` and `client` modules live in the layers `foundation` to
  `framework`, `tools` and `editor` modules in the `editor` layer. `runtime` depends only on
  `runtime`, `client` on `runtime` and `client`, `tools` on `runtime` and `tools`, `editor` on
  anything. The same test checks this.
- Executables are **applications** of one target: the editor links every kind, the player
  `runtime` and `client`, the dedicated server only `runtime`, tools `runtime` and `tools`. The
  test checks each application's whole dependency closure, and a CI job builds the sample player
  and server in Shipping with the other kinds turned off.
- The renderer, audio, physics and UI don't own the scene. They read data extracted from it
  each frame.
- No third-party or graphics-backend types appear in public module headers.

Foundation conventions: math types and coordinates follow
[ADR-0006](decisions/0006-math-and-coordinate-conventions.md), and logging and profiling follow
[ADR-0007](decisions/0007-logging-and-profiling.md).

## Modules today

| Module | Layer | Kind | What it is for | What it must not do |
|---|---|---|---|---|
| [`foundation`](../engine/ith/foundation) | foundation | runtime | Asserts (`ITH_ASSERT`, `ITH_VERIFY`, `ITH_FATAL`) with a replaceable handler; `ith::Expected<T, E>` for failures without exceptions; the engine version; string helpers; `Stopwatch` and `FixedTimestep`; FNV-1a hashing; `ith::Uuid`; the `LinearAllocator` arena and `ObjectPool<T>`; logging ([ADR-0007](decisions/0007-logging-and-profiling.md), [guide](development/logging.md); `Log.h`: `ITH_LOG_CATEGORY`, `ITH_LOG_INFO` and the other level macros, compile-time stripping, sinks, the stderr sink and the `LogRingBuffer` sink that keeps recent records for log views; failed asserts are logged as `Fatal`); `Mutex` and `MutexLock` with Clang thread-safety annotations (`ITH_GUARDED_BY` and friends) and a per-thread count of held locks (`heldMutexCount`); compiler and sanitizer hooks (`Compiler.h`) | Depend on anything but the C++ standard library; hold engine state |
| [`math`](../engine/ith/math) | foundation | runtime | Math types and spatial conventions ([ADR-0006](decisions/0006-math-and-coordinate-conventions.md), [guide](development/math.md)): glm 1.0.3 under engine names (`ith::Vec2/3/4`, `IVec2/3/4`, `UVec2/3/4`, `Mat3`, `Mat4`, `Quat`, `DVec3`, `DMat4`) in the engine's configuration (zero and identity defaults, `Quat(x, y, z, w)`, depth [0, 1], no swizzles or SIMD), carried by the `ith::glm` interface target; glm's functions through `ith/math/Math.h`; right-handed +Y up, −Z forward, metres and radians; `perspectiveReversedInfinite` (reversed depth, no far plane); `Transform`; `Aabb`, `Sphere`, `Plane`, `Frustum` (from any clip-space matrix) and `Ray` with culling and ray tests | Let any other directory include `<glm/…>` (the `glm_includes` test); set glm SIMD or alignment defines; use degrees or another handedness in code |
| [`platform`](../engine/ith/platform) | platform | client | Windows and the event loop on SDL ([ADR-0012](decisions/0012-windowing-and-event-loop.md), [guide](development/platform.md)): `ith::Platform` (initialization, the once-per-frame event pump with its pump time, UTF-8 clipboard, key display names in the user's layout, gamepad rumble, trigger rumble and LED), `ith::Window` (create and destroy, size in window coordinates and in pixels, display scale, relative mouse mode, text input and the IME area), `ith::NativeWindowHandle` (Win32, Wayland or X11 handles for the RHI), and the translation of SDL events into `platform_events` events: quit and close requests, resize, focus, minimize, maximize, restore, displays, low memory, background or foreground, keyboard, text and IME composition, mouse, and gamepads (opened and closed on hotplug). Main thread only, asserted | Expose SDL types; create graphics surfaces; run engine or game code inside the pump; be linked by a dedicated server |
| [`platform_events`](../engine/ith/platform_events) | platform | runtime | What the platform emits ([ADR-0012](decisions/0012-windowing-and-event-loop.md), [ADR-0013](decisions/0013-input.md), [ADR-0027](decisions/0027-input-rules-completed.md), [guide](development/input.md)): `ith::Event` (window, application and device events with monotonic nanosecond timestamps) and the stable device control ids: `Key` (physical keys, USB HID values), `MouseButton`, `GamepadButton`, `GamepadAxis`, `GamepadType`, `GamepadGuid`. Header-only, standard library only | Renumber an id; hold binding names or other vocabulary the platform doesn't emit; depend on SDL or any library |
| [`jobs`](../engine/ith/jobs) | platform | runtime | The job system ([ADR-0025](decisions/0025-job-system.md), [guide](development/jobs.md)): `ith::JobSystem`, created by the application and passed to its users, with a foreground lane (frame work, normal priority) and a background lane (long work, below-normal priority) of named worker threads and a main-thread queue (`postToMainThread`, `runMainThreadJobs`); jobs with inline captures of up to 48 bytes, from a fixed foreground pool and a growable background pool; `JobCounter`, continuations (`submitAfter`), `parallelFor` with worker-count-independent ranges; `wait` that runs queued jobs up to 8 nested waits, with asserted wait rules and a deadlock watchdog; a mode without workers for deterministic tests; `CancellationToken`, `Progress` and `FirstError`. Standard library only | Hold a global instance; run background jobs on foreground workers; allocate for foreground jobs; propagate exceptions; use standalone atomic fences |
| [`rhi`](../engine/ith/rhi) | platform | client | The rendering hardware interface ([ADR-0009](decisions/0009-rhi.md), [ADR-0021](decisions/0021-rhi-device-structure-and-backend-modules.md), [guide](development/rhi.md)), backend-neutral part: `rhi::Device` with `Capabilities` (backend, adapter, shader format, limits, optional mesh shaders, ray tracing, variable-rate shading, sampler feedback); 64-bit generational handles for buffers, textures, samplers, pipelines and swapchains with deferred destruction; bindless descriptor indices and 128-byte push constants; swapchains from `ith::NativeWindowHandle`; two frames in flight; `rhi::CommandList` with dynamic rendering, explicit enhanced-barrier/synchronization2-style barriers, draws, indirect draws with a count, dispatches and copies, checked in Debug and development builds; `rhi::Error` with device loss. Backends implement the internal `Backend.h` interface in their own modules; the null backend (`createNullDevice`, `NullGpu`) runs without a GPU, for tests and for the editor or player without one | Expose graphics-API or allocator types; depend on a backend module; track resource state; recreate a lost device; be linked by a dedicated server |
| [`rhi_d3d12`](../engine/ith/rhi_d3d12) | platform | client | The Direct3D 12 backend of the RHI, Windows only ([ADR-0009](decisions/0009-rhi.md), [guide](development/rhi.md#direct3d-12)): `createD3D12Device()` on the fastest hardware GPU or WARP with the ADR-0005 minimum check; one bindless resource heap and sampler heap with one root signature of 128 bytes of root constants; D3D12MA memory with persistently mapped upload and readback buffers; enhanced barriers; a direct queue with a fence timeline; flip-model DXGI swapchains; DXIL pipelines; the debug layer on request and DRED reports on device loss. Runs on the Agility SDK, which `ith_use_d3d12_agility_sdk()` puts next to executables | Expose Direct3D, DXGI or D3D12MA types; be depended on by anything but `rhi_select` and tests; be built outside Windows |
| [`rhi_vulkan`](../engine/ith/rhi_vulkan) | platform | client | The Vulkan backend of the RHI, Windows and Linux ([ADR-0009](decisions/0009-rhi.md), [guide](development/rhi.md#vulkan)): `createVulkanDevice()` on the best hardware GPU or a software one (lavapipe) with the ADR-0005 minimum check; Vulkan 1.3 with dynamic rendering and synchronization2 barriers; one update-after-bind, partially bound descriptor set and one pipeline layout of 128 bytes of push constants; VMA memory with persistently mapped upload and readback buffers; a timeline semaphore per queue; swapchains from Win32, X11 and Wayland windows; SPIR-V pipelines checked against the bindless set; the validation layer with synchronization validation on request, and `VK_EXT_device_fault` reports on device loss. Opens the system's Vulkan loader at run time and links no Vulkan library | Expose Vulkan or VMA types; be depended on by anything but `rhi_select` and tests; link the Vulkan loader |
| [`rhi_select`](../engine/ith/rhi_select) | platform | client | `createDevice()`, the RHI's backend selection ([ADR-0021](decisions/0021-rhi-device-structure-and-backend-modules.md)): Direct3D 12 by default on Windows, otherwise Vulkan (on request on Windows too), or the null backend; `rhi.backend_unavailable` for a backend that isn't built or can't start | Hold rendering code; register backends through static initializers |
| [`reflection`](../engine/ith/reflection) | core | runtime | Generated reflection ([ADR-0018](decisions/0018-reflection.md), [guide](development/reflection.md)): the `ITH_TYPE`, `ITH_FIELD` and `ITH_ENUM` annotations; `TypeInfo` and `FieldInfo` records with stable FNV-1a ids, layout, value functions (construct, copy, move, destroy, field-wise equality), flags and attributes; `ith::typeOf<T>()` with hand-written leaf types, the math types (`ith::Vec3`, `Quat`, `Mat4`, `Transform`, ...) and `std::array`, `std::vector`, `std::optional`; `TypeRegistry` with lookup by id, name and `FormerName`. Modules opt in with `REFLECT` in `ith_add_module` | Use RTTI or static initializers; register types after startup; hold editor-only data in runtime types |
| [`serialization`](../engine/ith/serialization) | core | runtime | Saving and loading any reflected value ([ADR-0010](decisions/0010-serialization.md), [guide](development/serialization.md)): the text archive (`writeText`, `readText`: pretty JSON with fields in declaration order, exact integers, shortest round-trip floats, unknown fields as warnings or errors, `FormerName`, comments only for config, the `id` of asset documents per ADR-0024; `readDocument` and `writeDocument` for documents keyed by names) and the binary archive (`writeBinary`, `readBinary`: a versioned little-endian blob with a schema hash and a hardened reader for cooked data, without `Transient` and `EditorOnly` data); `DocumentFormat` with versions and step-wise migrations of text documents; the `Value` tree with `parseJson` and `writeJson`. Reads are all-or-nothing and report `serialization::Error` with the path of the value. Uses yyjson privately | Expose yyjson types; do file I/O; migrate cooked data; read a blob whose version or schema hash differs |
| [`ecs`](../engine/ith/ecs) | core | runtime | The entity-component system ([ADR-0014](decisions/0014-ecs-storage-and-world.md), [ADR-0030](decisions/0030-ecs-hooks-registration-and-immutable-components.md), [guide](development/ecs.md)), slices 1–4: `ith::World` with a stable address, `ith::Entity` 64-bit generational handles (index, generation, world id) with stale-handle checks; archetype tables with aligned type-erased columns, swap-remove and cached add/remove transitions; constant-time `has`; typed queries (`query<T, const U>().forEach`) with per-world caches of matching archetypes. Component types are reflected structs with the `Component` role, identified by their reflection type id; `has`, `get`, `add` and `remove` also take a type's reflection record, `forEachComponent` lists an entity's components, and `dynamicQuery` (`ith::DynamicQuery`) iterates by records with read/write terms on the same matching code. World resources: typed singletons with the `Resource` role, by type or by record. Deferred commands (`ith::Commands`: spawn with a reserved handle, despawn, add, remove, append), applied in recording order by `World::apply`; buffers may be recorded by several jobs at once. The schedule (`ith::Schedule`): system descriptors with a name, a stage (`Input` to `Extract`), declared component and resource access and `before`/`after` constraints, checked by `build()`; a serial, deterministic executor with a sync point per stage that applies commands system by system in run order; `ith::SystemContext` asserts on undeclared access. Component hooks (`ith::setComponentHooks`: `onAdd`, `onInsert`, `onReplace`, `onRemove`, set once per process per type) that run in every world with an `ith::HookContext`, whose commands apply right after the change; immutable components (`ITH_TYPE(Component, Immutable)`), refused on every path that would write them in place | Expose its storage types in its API; reset generations; allow structural changes while a query iterates, a system or a hook runs; write an immutable component in place; depend on an ECS library |
| [`assets`](../engine/ith/assets) | core | runtime | Asset identity and references ([ADR-0015](decisions/0015-asset-identity-and-reimport.md), [guide](development/assets.md)): `AssetId` and `SubAssetId` (typed random `ith::Uuid`s), and the reflected hard `AssetRef` and soft `SoftAssetRef`, saved as ids only, with `assetReferenceKind` for tools that follow references. The runtime asset manager and cooked-data loader join it later | Store or resolve paths; derive ids from paths; depend on the asset database or importers |
| [`input`](../engine/ith/input) | core | runtime | Input routing and the binding vocabulary ([ADR-0013](decisions/0013-input.md), [ADR-0027](decisions/0027-input-rules-completed.md), [guide](development/input.md)): `ith::Input` with the device registry, per-frame `InputState` (pressed, released and held edges, taps within a frame, summed mouse motion and wheel, per-gamepad values), focus loss with real releases and axis resets, per-window consumer stacks (editor, UI, game) with consumption, text to the focused consumer, mouse capture and per-window capture on click, the frame's input time, and per-player device assignment (single-player default, explicit join); the stable control names (`controlName`, `findControl`: `Keyboard.W`, `Mouse.Delta`, `Gamepad.LeftStick`), the 1D mouse axes and the 2D controls. Never calls the platform layer, so it links into every process kind | Apply dead zones or curves; hold actions, mappings or bindings (the action plugin's job); call SDL or the platform layer; route events from inside a consumer's call |
| [`render_graph`](../engine/ith/render_graph) | engine | client | The render graph ([ADR-0028](decisions/0028-render-graph-v1.md), [guide](development/render-graph.md)): `rg::RenderGraph`, built every frame from raster, compute and copy passes with a setup callable that declares texture and buffer uses and an execute callable that records through `rg::PassContext` (asserting undeclared access); transient resources and imports with `ImportedState` written back (the backbuffer from `Undefined` to `Present`); a device-free compile in declaration order with culling from outputs and `NeverCull` passes, lifetimes, pool slots, RHI usages from the uses, whole-resource barriers batched per pass (out of `Undefined` after the previous holder's use), store operations from later use, and groups of passes; execute that records the groups into command lists, in parallel on a `JobSystem`, inside pass-name markers, with one submit; `rg::ResourcePool` reusing transients of equal descriptions within and across frames and destroying them after 4 unused frames; validation in Debug and development builds, clobbering of transients, and text and Graphviz dumps | Alias memory, reorder passes or use other queues (v1); expose graphics-API types; track subresources; let execute callables reach undeclared resources |
| [`input_actions`](../engine/ith/input_actions) | framework | runtime | The input action layer ([spec](specs/input-actions.md), [guide](development/input-actions.md)): input action and input context assets (`ith.input_action`, `ith.input_context`) with value kinds, the closed set of modifiers and triggers, and their validation (`InputAssetLibrary`); `InputActions`, which reads `ith::Input` through a Game-layer consumer and evaluates every local player (`InputPlayer`) once per frame: per-player context stacks with priorities and key claims, one edge per key per frame with copied edge queues, the five phases with event timestamps on Input's clock, chords and combos, focus loss, capture loss and suspension, action states (`ActionStates`) and per-frame `ActionEvent`s without callbacks, and key prompts; rebinding by stable slot names with profiles in four configuration layers (`InputBindingConfig`) and the `ith.input_bindings` document | Call the platform layer; evaluate anything but local players; allocate in steady state; assert on asset or configuration data; hold editor code |
| [`shader_compiler`](../engine/ith/shader_compiler) | editor | tools | `ith::ShaderCompiler`: compiles a self-contained Slang source for one entry point and stage to SPIR-V 1.6 or to signed DXIL Shader Model 6.6, through Slang's library API and DXC, with the compiler diagnostics as the error ([ADR-0011](decisions/0011-shader-language-and-pipeline.md), [guide](development/shaders.md)). Built only with `IRONTHORN_BUILD_TOOLS` | Be linked by a game or any runtime module; expose Slang types |
| [`asset_database`](../engine/ith/asset_database) | editor | tools | The asset database of a project ([ADR-0015](decisions/0015-asset-identity-and-reimport.md), [guide](development/assets.md)): `<file>.meta` sidecars (id, importer and version, settings, sub-asset table by stable source key, overrides); native asset headers (`.ithasset`, levels `.ithmap`); `AssetDatabase` with the scan of `Content/` and its problem list (duplicate ids, orphan and missing sidecars, case conflicts, unreadable files and folders, extension and format mismatches), move, rename and delete with sidecars, and import and non-destructive reimport; the `Importer` interface and `ImporterRegistry`; the local derived-data cache with 128-bit content keys (`hash128`, XXH3-128). Uses xxHash privately. Built only with `IRONTHORN_BUILD_TOOLS` | Be linked by a game or any runtime module; expose xxHash types; create sidecars during a scan, or change ids on a scan without history |
| [`asset_importers`](../engine/ith/asset_importers) | editor | tools | The importers of source files ([ADR-0015](decisions/0015-asset-identity-and-reimport.md) rule 8, [guide](development/assets.md#the-gltf-importer)): `GltfImporter` for `.gltf` and `.glb` (embedded, GLB and external buffers, every accessor form, triangle meshes, metallic-roughness materials, texture images recorded as references), and the versioned payloads `ImportedMesh` and `ImportedMaterial`. Our own reader on the serialization module's JSON; a fuzz target. Built only with `IRONTHORN_BUILD_TOOLS` | Be linked by a game or any runtime module; read files itself (the asset database passes them); trust any offset, length or index in a source file |

Build tools in `tools/` run on the host during the build and are linked into nothing.
`ith_reflect` scans the annotated headers of each module that sets `REFLECT` and writes its
`<module>.reflect.cpp`; it needs no third-party code and is built in every configuration.

Sample applications in `samples/`: `minimal_player` (player) and `minimal_server` (dedicated
server) create an empty world and exit. `window_player` (player) opens a window, runs the
event loop and clears the window through the RHI where it can render (Direct3D 12 on Windows,
Vulkan elsewhere, or the backend `--backend d3d12|vulkan|null` names, on the GPU
`--adapter hardware|software` names),
until the window is closed or for `--frames N` frames. `scene_player` (player, built with the
tools) draws through the RHI's bindless model in a render graph pass: 10,000 instanced cubes in one
draw with a depth buffer, or a triangle (`--scene triangle`), with the same options and
`--no-vsync`, and prints its frame timings ([RHI guide](development/rhi.md#samples)). They give the
target check and the shipping link test players and a server to build.

Test support (`tests/support`) provides the doctest entry point and `ith::test::AssertCapture`,
which records failed asserts so tests can check them.

## World model

- The **ECS** stores entities in archetype tables, with relations, deferred commands, change
  detection and a scheduler of systems in stages: serial and deterministic first, parallel over
  the declared access once profiles show the gain.
- The editor shows the ECS as an ordered hierarchy of entities with components in the Outliner.
  "Actor" is an authoring word for an entity with a transform. See [Editor](#editor).
- **Levels** are assets, and a **world** instantiates them. Open worlds stream in cells.
- **Prefabs** nest, and changes to a prefab reach the copies that haven't overridden them.
- Gameplay is ECS-native ([ADR-0016](decisions/0016-gameplay-model-on-the-ecs.md)): components
  are data, systems hold the logic, resources hold world-wide and app-wide state, and relations
  link entities. There is no runtime Actor class and no per-entity tick. Players are entities
  with a `Player` component, control is a `Controls` relation, and match rules are systems plus
  a rules resource. Unreal concepts such as pawn, controller and game mode map onto these, as
  the ADR's table shows.
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
| P0 | Repository, build, CI, foundation module, first ADRs | done |
| P1 | Platform, jobs, RHI (Vulkan, D3D12), render graph, reflection, serialization, assets, input | done |
| P2 | Editor shell following Unreal Engine workflow conventions ([ADR-0008](decisions/0008-editor-familiar-to-unreal-developers.md)): viewport, outliner, details, content browser, dockable saved layouts, remappable input with the Unreal-style default preset, undo, play-in-editor, prefabs | **next** |
| P3 | Renderer v1 and the scalability framework | planned |
| P4 | Game UI framework and designer | planned |
| P5 | Gameplay core: physics, animation, audio, scripting, abilities, networking, dedicated server | planned |
| P6 | Gameplay kit: inventory, interaction, equipment, crafting, quests, dialogue, save | planned |
| P7 | Open world: streaming, terrain, foliage, water, sky and weather, navigation, AI | planned |
| P8 | Renderer v2: volumetrics, GPU particles, probe GI with bake-down, virtual shadow maps | planned |
| P9 | Production hardening: packaging, crash reporting, localization, accessibility, profiler | planned |
| P10 | 2D toolset | planned |

Releases follow phases ([ADR-0002](decisions/0002-git-workflow-and-versioning.md)): version
0.1.0 completed P1.
