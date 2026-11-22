# Changelog

All notable changes to Ironthorn are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Unreleased changes are kept as fragments in [changelog.d/](changelog.d/) and gathered here when
a version is released ([ADR-0002](docs/decisions/0002-git-workflow-and-versioning.md)).

## [0.1.0] - 2026-11-22

The first release. It completes phase P1 of the [roadmap](docs/architecture.md#roadmap): the
engine's foundation, without an editor, renderer or gameplay yet.

### Added

#### Repository and build

- The Ironthorn repository under the Apache License 2.0: CMake build with presets for MSVC,
  clang-cl, Clang and GCC, sanitizer builds, the `scripts/dev.ps1` developer entry point and CI
  on Windows and Linux.
- Module layering and changelog fragment checks that run as tests.
- Modules declare a kind with `ith_add_module(... KIND runtime|client|tools|editor)` (default
  `runtime`). The `module_layering` test checks which kinds may depend on which, and that each
  kind sits in the right layers.
- `ith_add_application(<name> KIND editor|player|server|tools ...)` declares an executable; the
  `module_layering` test checks that everything it links is allowed for its target. Editor and
  tools applications are not part of the Shipping (`Release`) build.
- The CMake options `IRONTHORN_BUILD_CLIENT`, `IRONTHORN_BUILD_TOOLS` and `IRONTHORN_BUILD_EDITOR`
  (on by default) build the client, tools-only and editor modules. The tools download their
  pinned dependencies.
- The presets `linux-shipping-player` and `linux-shipping-server` build the sample player and
  server in Shipping without the modules they must not link.
- Building the client modules on Linux needs the X11, Wayland and related development packages
  listed in `docs/development/building.md`.
- Clang AddressSanitizer builds compile everything with libFuzzer's coverage instrumentation, so
  fuzz targets get coverage feedback from the engine code they reach.
- Documentation: architecture overview, ADRs, coding standards, building, contributing, security
  policy, and a guide for each module.

#### Foundation

- `ith/foundation`: `ITH_ASSERT`/`ITH_VERIFY`/`ITH_FATAL` with a replaceable handler,
  `ith::Expected<T, E>` for errors without exceptions, and the engine version
  (`ith::kEngineVersion`).
- `ith/foundation/LinearAllocator.h`: `ith::LinearAllocator`, a fixed-capacity arena with
  markers, for per-frame scratch data.
- `ith/foundation/ObjectPool.h`: `ith::ObjectPool<T>`, a chunked pool of fixed-size slots whose
  `create` returns an owning `std::unique_ptr` that gives the slot back to the pool.
- `ith/foundation/Hash.h`: stable, `constexpr` 64-bit FNV-1a hashing of strings and bytes, and
  `ith::hashCombine` for composite keys.
- `ith/foundation/StringUtils.h`: locale-independent string helpers (`trim`, `split`,
  `toLowerAscii`, `equalsIgnoreCaseAscii`, `endsWithIgnoreCaseAscii`, `replaceAll`,
  `hexDigitValue`) and `ith::isValidUtf8`, which checks that text is well-formed UTF-8.
- `ith/foundation/Uuid.h`: `ith::Uuid`, 128-bit identifiers with thread-safe random (version 4)
  generation, canonical text conversion and parsing that reports malformed text through
  `ith::Expected`.
- `ith/foundation/Stopwatch.h`: `ith::Stopwatch`, a monotonic stopwatch.
- `ith/foundation/FixedTimestep.h`: `ith::FixedTimestep`, which turns variable frame times into
  fixed simulation steps without a spiral of death.
- `ith/foundation/Mutex.h`: `ith::Mutex` and `ith::MutexLock`, a mutex that Clang's thread-safety
  analysis understands, and the annotation macros `ITH_GUARDED_BY`, `ITH_REQUIRES`,
  `ITH_EXCLUDES`, `ITH_ACQUIRE`, `ITH_RELEASE`, `ITH_CAPABILITY` and `ITH_SCOPED_CAPABILITY` in
  `ith/foundation/Compiler.h`. In builds with asserts, `ith::heldMutexCount()` returns how many
  `ith::Mutex` objects the calling thread holds, so code that must not block while holding a lock
  can assert it.

#### Platform and jobs

- `ith/platform`: windows and the event loop on SDL 3.4. `ith::Platform` initializes the
  platform layer, creates windows, pumps the operating system's events once per frame into a
  list of engine events (quit and close requests, resize, display scale, focus, minimize,
  maximize, restore, displays, low memory, background and foreground) and reads and writes the
  clipboard. `ith::Window` reports its size in window coordinates and in pixels, its display
  scale and its native handles for the RHI. See `docs/development/platform.md`.
- `ith/jobs`: the job system. `ith::JobSystem` runs jobs on a foreground lane for frame work and
  a background lane for long work, each with its own named worker threads (background below
  normal OS priority), and on the main thread through `postToMainThread` and
  `runMainThreadJobs`. Jobs capture up to 48 bytes inline and foreground jobs never allocate.
  `JobCounter` and `wait` (which runs queued jobs while waiting, up to 8 nested waits),
  continuations with `submitAfter`, `parallelFor` with ranges that don't depend on the worker
  count, asserted wait rules and a deadlock watchdog, and a mode without workers for
  deterministic tests. `CancellationToken`, `Progress` and `FirstError` support long
  operations. See `docs/development/jobs.md`.
- `ith_jobs_benchmark` measures empty-job overhead, `parallelFor` speed-up, continuation latency
  and wake-up latency.

#### Rendering hardware interface

- `ith/rhi`: the backend-neutral rendering hardware interface. `rhi::Device` creates buffers,
  textures, samplers, graphics and compute pipelines and swapchains, refers to them by
  generational handles that go stale on `destroy()`, and releases them only after the GPU and
  every command list open at the time are done with them. Shaders reach resources through
  bindless indices and 128-byte push constants. Frames run two in flight; swapchains are created
  from a window's native handle and recreated after `resizeSwapchain()` or when out of date.
  `rhi::CommandList` records dynamic rendering, explicit barriers, draws (indexed, and indirect
  with a count), dispatches and copies, from several threads at once, with checks in Debug and
  development builds. `CommandList::copyTextureToBuffer()` reads rendered pixels back. A storage
  texture with an sRGB format is refused with `rhi.invalid_usage`. Failures, device loss
  included, are `rhi::Error` values. See `docs/development/rhi.md`.
- `ith/rhi/NullDevice.h`: a null backend without a GPU, for tests and for running the editor or
  player without one, with a simulated GPU timeline and fault injection.
- `ith/rhi_d3d12`: the Direct3D 12 backend on Windows. `createD3D12Device()` creates a device on
  the fastest hardware GPU or on WARP, refuses GPUs without Shader Model 6.6, Resource Binding
  Tier 3, wave intrinsics or enhanced barriers, and runs bindless resources, enhanced barriers,
  DXIL pipelines, indirect draws and dispatches, flip-model swapchains, the debug layer and DRED
  device-loss reports.
- Player and editor applications on Windows that link `rhi_select` run on the Direct3D 12 Agility
  SDK 1.620.2, whose runtime the build copies next to them (`ith_use_d3d12_agility_sdk()`).
- `ith/rhi_vulkan`: the Vulkan backend, on Windows and Linux. `createVulkanDevice()` creates a
  Vulkan 1.3 device on the best hardware GPU or a software one (lavapipe) after the ADR-0005
  minimum check, with dynamic rendering, synchronization2 barriers, one bindless descriptor set,
  128-byte push constants, VMA memory, timeline semaphores, swapchains for Win32, X11 and Wayland
  windows, and `VK_EXT_device_fault` reports on device loss. It opens the system's Vulkan loader
  at run time and links no Vulkan library. In Debug builds it runs the Khronos validation layer
  with synchronization validation when the layer is installed.
- `ith/rhi_select/CreateDevice.h`: `createDevice()` picks the backend: Direct3D 12 by default on
  Windows, Vulkan by default elsewhere and on request on Windows, or the null backend.

#### Shaders and render graph

- `ith/shader_compiler/ShaderCompiler.h`: `ith::ShaderCompiler`, a tools-only module that compiles
  Slang shader source to SPIR-V 1.6 and to signed DXIL Shader Model 6.6 through Slang 2026.22.1
  and DXC 1.9.2609, reporting compiler diagnostics through `ith::Expected`. It keeps entry-point
  names in SPIR-V and defines `ITH_SPIRV=1` when it compiles SPIR-V, so one Slang source can
  declare the Vulkan bindless set for SPIR-V and use `ResourceDescriptorHeap` for DXIL. The
  compile options are listed in `docs/development/shaders.md`.
- `ith_embed_shaders()` compiles Slang entry points at build time with the pinned `slangc` into
  headers a sample or test embeds, as SPIR-V and optionally DXIL.
- `ith/render_graph`: render graph v1 (ADR-0028). `rg::RenderGraph` takes raster, compute and
  copy passes, each a name, a setup callable that declares texture and buffer uses, and an
  execute callable that records through `rg::PassContext`. It culls passes whose writes reach no
  output, derives every barrier from the declared uses (batched per pass, out of `Undefined`
  after the previous use, as `Barrier.h` requires), begins and ends rendering with store
  operations chosen from later use, and records the passes in groups into command lists, in
  parallel on a `JobSystem`, inside pass-name markers. Transient resources come from an
  `rg::ResourcePool` that reuses equal descriptions within and across frames and frees them after
  4 unused frames; imports keep their state between frames in `rg::ImportedState`, and
  `importBackbuffer()` takes the swapchain image to `Present`. Validation in Debug and development
  builds names the graph, pass and resource; transients can be clobbered to show stale reads;
  `dump()` and `dumpGraphviz()` print the plan. See `docs/development/render-graph.md`.
- `ith_render_graph_benchmark` reports declare, compile and execute times of 100 passes.

#### Reflection and serialization

- `ith/reflection`: `ith::typeOf<T>()` and `ith::TypeRegistry` describe reflected types with
  stable FNV-1a ids, field offsets and types, construct, copy, move, destroy and field-wise
  equality, whether values are trivially copyable (`ith::TypeInfo::triviallyCopyable`), flags and
  attributes, and find types by id, name or `FormerName`. Shipping builds compile out tooltips,
  categories, display names and `Meta`. See `docs/development/reflection.md`.
- `ith_reflect`, a build tool in `tools/`, generates a module's reflection records from its
  headers annotated with `ITH_TYPE`, `ITH_FIELD` and `ITH_ENUM`, and refuses unsupported syntax
  with `file:line` errors. Modules opt in with `REFLECT` in `ith_add_module`; test libraries use
  `ith_reflect_target`.
- `ith/serialization`: saving and loading any reflected value. `writeText` and `readText` use
  pretty JSON with fields in declaration order, exact integers and floats that read back
  bit-identical; unknown fields are warnings (or errors when cooking), `FormerName` is accepted,
  and comments are allowed only for config files. `writeBinary` and `readBinary` use a versioned
  little-endian blob for cooked data, without `Transient` and `EditorOnly` data, checked by a
  schema hash and read by a hardened reader. Documents carry a format name and version, and text
  documents migrate step by step. Reads are all-or-nothing and report errors with the path of the
  value, e.g. `$.history[1].where.x`, and allocate at most a budget (by default the larger of
  64 MiB and 16 times the input size). See `docs/development/serialization.md`.
- Asset documents carry their asset id in the envelope; `readText` and `writeText` read and write
  it, and the caller says whether it expects an asset document (`TextReadOptions::id`). Any other
  document with an `"id"` is refused with `serialization.not_a_document`. `readDocument` and
  `writeDocument` read and write documents whose data is keyed by names, such as configuration.
- `ith/serialization/Json.h`: `ith::serialization::Value`, a JSON value tree, with `parseJson`
  and `writeJson`.
- The engine depends on yyjson 0.13.0 (MIT), used only inside the `serialization` module.

#### ECS and assets

- `ith/ecs`: `ith::World` stores entities and their components in archetype tables. Entities are
  64-bit generational handles (`ith::Entity`) that go stale when despawned or when the world is
  cleared; `spawn`, `despawn`, `add`, `remove`, `get` and constant-time `has`; typed queries
  (`world.query<Position, const Velocity>().forEach(...)`) with cached archetype matches.
  Component types are reflected structs with the `Component` role. See `docs/development/ecs.md`.
- `ith_ecs_benchmark` measures spawning, despawning, iteration, tag churn and random access over
  100k entities.
- `ith/assets`: `ith::AssetId` and `ith::SubAssetId`, and the reflected reference types
  `ith::AssetRef` (hard) and `ith::SoftAssetRef` (soft), which save ids only.
- `ith/asset_database` (tools only): `<file>.meta` sidecars, the headers of native assets
  (`.ithasset`, levels `.ithmap`), and `ith::AssetDatabase`, which scans a project's `Content`
  folder, reports duplicate ids, orphan and missing sidecars, case conflicts and unreadable
  files, moves, renames and deletes assets with their sidecars, and imports through the
  `ith::Importer` interface. Reimport keeps the ids of sub-assets by their stable source keys and
  marks disappeared ones missing. Importer output is cached in a local derived-data cache keyed by
  XXH3-128 content hashes.
- `ith::Importer::dependencies()` names the files an importer reads besides its source; the asset
  database reads them from the source's folder, includes them in the derived-data key and passes
  them to `import()` as a `std::span<const ith::ImportFile>`. `ImportOutput::references` and
  `ImportReport::references` list the files the imported objects refer to, such as a material's
  images.
- `ith/asset_importers` (tools only): `ith::GltfImporter` imports glTF 2.0 `.gltf` and `.glb`
  files: buffers in the GLB `BIN` chunk, base64 `data:` URIs or files next to the source, sparse,
  interleaved and normalized accessors, triangle meshes (positions, normals, first texture
  coordinates, indices) as `ith::ImportedMesh` sub-assets and metallic-roughness materials as
  `ith::ImportedMaterial` sub-assets. Meshes without a name are keyed by the name path of the
  node that uses them. Every offset, length, stride and index is checked; names and paths are
  limited to 4 KiB; and everything the import builds, copies of names and paths included, is
  limited to 16 times the input (at least 64 MiB, at most 4 GiB).

#### Input

- `ith/platform_events`: the engine's event type (`ith::Event`) and the stable ids of keys, mouse
  buttons, positional gamepad buttons, raw gamepad axes and gamepad types.
- `ith/platform`: keyboard (physical key, layout key, modifiers, repeats), text input with IME
  composition, mouse (buttons, position in pixels, motion, fractional wheel with the natural
  scrolling setting) and gamepad events (hotplug, name, type, GUID, Steam handle, buttons, sticks
  and triggers); gamepad rumble, trigger rumble and LED; key names in the user's layout; relative
  mouse mode, text input and the IME area on `ith::Window`; the pump time of each frame. A machine
  without gamepad support still gets windows, keyboard and mouse.
- `ith/input`: `ith::Input` builds per-frame input state from the events (taps within a frame,
  repeats that aren't presses, releases on focus loss) and routes them through per-window
  consumer stacks with consumption, focus, mouse capture, capture on click and per-player device
  assignment. The stable control names (`Keyboard.W`, `Mouse.Delta`, `Gamepad.LeftStick`) are
  found with `ith::findControl`. See `docs/development/input.md`.
- `ith/input_actions`: the input action layer. Input action and input context assets
  (`ith.input_action`, `ith.input_context`) with digital, 1D, 2D and 3D values, dead zone, scale,
  negate, swizzle, curve, frame-time and user-setting modifiers, and Down, Pressed, Released, Hold,
  HoldRelease, Tap, MultiTap, Pulse, Chord and Combo triggers; validation of missing actions,
  unknown keys and settings, slot conflicts and dependency cycles. `ith::InputActions` evaluates
  every local player each frame: context stacks with priorities and key claims, presses shorter
  than a frame kept, exact timestamps, focus loss, mouse capture loss and suspension, and writes
  `ith::ActionStates` and per-frame `ith::ActionEvent`s for systems to read. Players rebind keys by
  stable slot names in profiles stored in layered configuration (`ith.input_bindings`). See
  `docs/development/input-actions.md`.

#### Samples

- `minimal_player` and `minimal_server` create an empty world and exit, which proves that a player
  and a server link without editor or tools modules.
- `window_player` opens a window and runs until it is closed, or for `--frames N` frames. It
  clears the window to a slowly changing color through the RHI, picks the backend with
  `--backend d3d12|vulkan|null` and the GPU with `--adapter hardware|software`, and runs headless
  with `SDL_VIDEO_DRIVER=offscreen`. The `linux-shipping-player` preset builds it in Shipping.
- `scene_player` draws through the render graph and the RHI's bindless model: 10,000 instanced
  cubes in one indexed draw with a pooled transient depth buffer (the default), or a triangle
  (`--scene triangle`), on Direct3D 12 or Vulkan (`--backend`, `--adapter`, `--frames`,
  `--no-vsync`), and prints its frame timings. It is built with the tools, which compile its
  shaders.
- The `scene_player` GPU test renders the sample's scenes offscreen on every backend the machine
  has, checks them against the geometry, and compares Direct3D 12 with Vulkan.

[0.1.0]: https://github.com/Bitrose-Studio/Ironthorn/releases/tag/v0.1.0
