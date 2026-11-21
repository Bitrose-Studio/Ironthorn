# ADR-0028: Render graph v1

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions or RTTI,
  `ith::Expected`), [ADR-0003](0003-clean-room.md), [ADR-0004](0004-modules-and-layering.md)
  (layers), [ADR-0005](0005-reference-hardware.md) and
  [ADR-0022](0022-gpu-minimum-features-completed.md) (GPU features, tiers),
  [ADR-0009](0009-rhi.md) and [ADR-0021](0021-rhi-device-structure-and-backend-modules.md)
  (explicit barriers, frames, deferred destruction, the null backend),
  [ADR-0011](0011-shader-language-and-pipeline.md) and
  [ADR-0023](0023-build-time-shaders-for-samples-and-tests.md) (shaders),
  [ADR-0017](0017-runtime-and-editor-separation.md) (module kinds),
  [ADR-0025](0025-job-system.md) and [ADR-0026](0026-job-system-rules-corrected.md) (jobs),
  [RHI guide](../development/rhi.md)

## Context

ADR-0009 rule 11 makes barriers explicit and leaves computing them to a render graph. Until it
exists, passes write them by hand: the `scene_player` sample spells out the backbuffer's
transitions and a `depthBarrier()` that must name the previous frame's depth writes, or Vulkan's
synchronization validation reports a hazard (the `Undefined` rule in `Barrier.h`). That is
manageable for one pass and fragile for twenty.

The roadmap puts render graph v1 in P1 and the first real renderer in P3: an opaque pass (forward
or deferred) with depth, post and tonemap, present; then shadows, and later async compute. The
plan's core feature, scalability that keeps the look, needs two things from the graph later:
passes that can be dropped cheaply and per-pass GPU cost, and rendering at an internal resolution
below the output. v1 must not block either.

What the RHI offers today, and what it doesn't:

- explicit stage/access/layout barriers, batched in one call (`rhi::Barriers`);
- dynamic rendering with load and store operations; whole-resource storage views at mip 0;
- command lists from per-frame pools, recordable on several threads, submitted in order with
  `submit(span)`; one graphics queue;
- every buffer and texture is a committed allocation through VMA or D3D12MA. There are **no
  heaps or placed resources**, so memory aliasing of transient resources is not possible
  without an RHI extension;
- read-only depth attachments are not implemented on Vulkan yet (RHI guide, "Not yet").

Lessons from earlier engine prototypes by the same maintainer:

- A renderer without a graph kept every intermediate texture of a view (G-buffer, AO, bloom
  levels, scene color, display, about fifteen) in a hand-owned per-view struct, resized together
  and alive all frame. Two of them had to survive between frames (exposure and motion history).
  Memory was never shared, and every new pass meant editing that struct.
- An RHI that tracked resource state for the renderer hid the cost of transitions and made it
  impossible to reason about them in one place; ADR-0009 already rejects that model.

Public prior art, read for ideas only (ADR-0003):

- Frostbite's FrameGraph (Yuriy O'Donnell, GDC 2017,
  [GDC Vault](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in),
  [slides](https://www.slideshare.net/DICEStudio/framegraph-extensible-rendering-architecture-in-frostbite)):
  setup, compile and execute phases, a graph rebuilt every frame, culling of unused passes,
  transient resources with aliased memory. Only the slides' outline was checked, not the
  full talk.
- Unreal Engine's Render Dependency Graph
  ([Epic documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)):
  passes declare resources through a parameter struct and record in a deferred lambda; transient,
  registered (external) and extracted resources; pass culling; parallel recording; a transient
  allocator that aliases memory, with descriptor-matched pooling as its fallback. Its debugging
  aids are an immediate mode, a validation layer in development builds that names pass and
  resource, a transition log and a timeline of lifetimes. The page warns that pooling hides reads
  of a transient's stale contents, which aliasing then exposes.
- "Render graphs and Vulkan — a deep dive" (Hans-Kristian Arntzen, 2017,
  [blog](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/)):
  dependency walk back from the backbuffer, barriers built from first and last use, reuse of
  physical images with equal size and format, history images that never alias. It also notes
  that reordering heuristics are costly and that async compute brought subtle bugs.

## Options considered

**A. No graph: hand barriers plus a small state tracker in the renderer.** The least code now.
Every pass still has to know who used a resource before it, nothing culls or pools, and
parallel recording needs the tracker's state to be split by hand. It is ADR-0009's interim
state, not an end state.

**B. A third-party render graph library.** The C++ ones found are renderer-agnostic and leave
barriers and allocation, the hard parts, to the user. For example
[skaarj1989/FrameGraph](https://github.com/skaarj1989/FrameGraph) (MIT, C++20, no releases,
last commit `f84d671` on 2025-12-03) uses `typeid`, `std::any` and `dynamic_cast` (checked in its
headers), which ADR-0001 forbids, and allocates each pass with `std::unique_ptr`. Wrapping it
would cost more than the part it saves.

**C. A full graph now:** memory aliasing, async compute, pass reordering and subresource
tracking, as mature engines have. Aliasing needs heaps and placed resources in the RHI (a new
ADR and both backends), async compute needs a second queue and cross-queue synchronization, and
none of it pays off before P3 has passes to measure. Most of the risk, little of the value yet.

**D. A minimal own graph** (chosen): declare, compile, execute; declaration order; culling;
barriers derived from declared uses; pooled transient resources; imported resources whose state
the graph carries between frames; parallel recording through the job system. Each feature of C
can be added behind the same declarations when a measurement asks for it.

## Decision

### Module and lifetime

1. **A new module `render_graph`** (layer `engine`, kind `client`), depending on `rhi`, `jobs` and
   `foundation`; headers under `ith/render_graph/`, namespace `ith::rg`. The 3D renderer, the
   2D renderer, the UI renderer and the editor viewport depend on it, as declared same-layer
   edges (ADR-0004 rule 3). No graphics-API type and no allocation per pass from the general
   heap: the graph's nodes and pass callables live in a per-graph arena reset each frame.
2. **A graph is built every frame** (`RenderGraph`), compiled and executed once, then reset.
   A long-lived **`ResourcePool`**, one per device, owned by the renderer, holds the transient
   resources between frames. All views of a frame (the game, editor viewports) add their passes
   to the same graph.

### Declaring passes

3. **A pass is a name, a type, a setup callable and an execute callable:**

   ```cpp
   graph.addPass("Tonemap", rg::PassType::Raster,
       [&](rg::PassBuilder& pass) {             // runs now; may capture by reference
           pass.read(sceneColor, rg::TextureUse::Sampled);
           pass.renderTarget(backbuffer, rhi::LoadOp::DontCare);
       },
       [sceneColor, pipeline](rg::PassContext& ctx) {  // runs at execute; captures by value
           ctx.list().setPipeline(pipeline);
           ctx.list().pushConstants(TonemapConstants{.input = ctx.sampledIndex(sceneColor)});
           ctx.list().draw(3);
       });
   ```

   Pass types are `Raster`, `Compute` and `Copy`. Names are required and non-empty. **Why
   lambdas, not parameter structs:** declarations stay next to the code that uses them, without
   reflection or macros, and a struct-based form can be layered on top if passes grow many
   parameters. The execute callable is stored in the graph's arena, type-erased without RTTI,
   and destroyed at reset.
4. **Resources are graph handles**, small typed values (`rg::Texture`, `rg::Buffer`) valid for
   one graph. Kinds:
   - **transient**, created by the graph from a description (`createTexture`, `createBuffer`):
     size, format, mips, layers, debug name. The RHI usage flags are not given: the graph takes
     the union of the uses declared on the resource;
   - **imported**, an RHI resource the caller owns (`importTexture`, `importBuffer`): the
     swapchain image, persistent history, upload buffers, asset textures. An import names the
     resource's state at the start of the graph and whether the graph may write it. The caller
     keeps an `rg::ImportedState` (layout, last stages, last accesses) per persistent resource;
     the graph reads it at import and writes the final state back at execute, so a history
     texture's barrier in the next frame orders after this frame's use. The backbuffer is
     imported each frame from `Undefined` with a required final layout of `Present`.
5. **Uses** map one to one onto the RHI's stages, accesses and layouts:

   | Use | Layout | Stages, access |
   |---|---|---|
   | `Sampled` (texture) / `ShaderRead` (buffer) | `ShaderRead` | the pass's shader stages, `ShaderRead` |
   | `Storage` | `ShaderWrite` | shader stages, `ShaderRead`/`ShaderWrite` |
   | `RenderTarget` | `RenderTarget` | `ColorOutput`, `RenderTargetWrite` (and read for `Load`) |
   | `DepthWrite` | `DepthStencilWrite` | `DepthStencil`, `DepthStencilRead`/`Write` |
   | `DepthSampled` | `DepthStencilRead` | shader stages, `ShaderRead` |
   | `CopySrc` / `CopyDst` | `CopySrc` / `CopyDst` | `Copy`, `CopyRead` / `CopyWrite` |
   | `Indirect` (buffer) | — | `DrawIndirect`, `IndirectArgument` |
   | `Index` (buffer) | — | `IndexInput`, `IndexBuffer` |

   The shader stages are the pass type's (`VertexShader | PixelShader` for raster,
   `ComputeShader` for compute) unless the declaration narrows them. A read-only depth
   attachment use is added when the RHI supports it on Vulkan.
6. **Raster passes declare their attachments** (`renderTarget`, `depth`) with a load operation
   and clear value. The graph calls `beginRendering` and `endRendering` around the execute
   callable and sets the viewport and scissor to the attachments' size. It chooses the store
   operation: `DontCare` when no later pass reads the attachment and it is not imported.
7. **Side effects.** A pass that writes an imported resource is an output. A pass whose effect
   the graph can't see (a readback, a GPU timestamp) is marked `PassFlags::NeverCull`.
8. **Execute callables reach RHI handles only through `PassContext`** (`texture()`, `buffer()`,
   `sampledIndex()`, `storageIndex()`, `shaderReadIndex()`, `shaderWriteIndex()`), which assert
   in Debug and Development builds that the pass declared the resource with a matching use.
   Execute callables may run on any thread and in parallel with each other: they must not write
   shared state.

### Compiling

9. **Order is declaration order.** v1 does not reorder passes. A read of a transient resource
   that no earlier pass wrote is an error. **Why:** order the author can read in the code, no
   reordering heuristic, deterministic output.
10. **Culling** walks back from outputs (rule 7): a pass none of whose writes reaches an output is
    removed, and so are transient resources only culled passes use. Culling can be turned off for
    debugging.
11. **Barriers are derived per resource, whole-resource in v1.** The compiler follows each
    resource's uses in order and emits a barrier before a use when the layout changes or either
    side writes; consecutive reads in the same layout merge their stages and need none. All
    barriers before a pass go in one `rhi::Barriers` batch.
    - A transient's first use in a frame, and any use whose load operation is `Clear` or
      `DontCare`, transitions from `Undefined`, naming the stages of the resource's previous use
      and, if it wrote, its accesses, as `Barrier.h` requires. The previous use is the earlier
      holder of the pooled resource, in this frame or an earlier one (rule 13).
    - An imported resource starts from its import state and ends in its required final state.
    - Subresource (per-mip, per-layer) tracking is added when a pass needs it; v1 passes use
      separate textures, as the RHI's mip-0 storage views already suggest.
12. **Compile is separate from allocation and needs no device.** It turns declarations into a
    plan: pass order, culled passes, each resource's first and last use, its RHI usage, and the
    barriers as abstract transitions. The same declarations always give the same plan.

### Resources and memory

13. **Transient resources are pooled, not aliased, in v1.** At execute, each transient takes a
    pooled RHI resource whose description and usage match exactly, at its first use, and gives it
    back after its last use, so two transients with equal descriptions and disjoint lifetimes
    share one resource within a frame, and the same resources serve the next frames. The pool
    remembers each resource's last use for the next `Undefined` transition. A pooled resource
    unused for a few frames (default 4, a setting) is destroyed through the RHI's deferred
    destruction. Out-of-memory from creation fails `execute` with `rhi.out_of_memory`.
14. **Memory aliasing** (placed resources in shared heaps, sharing memory between different
    descriptions) is later work. It needs heap and placed-resource support in the RHI, which is
    an amendment to ADR-0009 with its own tests, and is started when the P3 renderer's memory
    watermark shows a gain on the Low tier's 4 GB budget (ADR-0005). The declarations of rules
    3–5 don't change when it lands.
15. **Debug option: clobber transients.** A Debug and Development switch clears each transient to
    a loud value at its first use, so a pass that reads stale pooled contents shows it before
    aliasing would.

### Executing

16. **One graphics queue in v1.** Async compute and copy queues come with RHI queue support and
    a profile that shows a gain (ADR-0009 rule 13).
17. **Recording in groups of passes, in parallel when a `JobSystem` is given.** The plan is cut
    into groups of consecutive passes, each recorded into its own command list with its barriers,
    and the lists are submitted in plan order with one `submit(span)`. Group boundaries depend on
    the plan only, never on the worker count (as ADR-0025 rule 11), so serial and parallel
    execution record the same commands. With no job system, or one with zero workers, groups are
    recorded in order on the calling thread. Execute is called on the render thread inside an open
    frame; recording jobs run in the foreground lane and are waited for before submission.
18. **Every pass records inside a marker** with its name (`beginMarker`/`endMarker`), so PIX,
    RenderDoc and the API validation messages name the pass. Per-pass GPU timing hooks into the
    same place when the RHI gains timestamp queries; it is the unit the bottleneck classifier and
    the Scalability Lab measure.

### Scalability hooks (not built in v1)

19. v1 adds nothing for scalability but keeps two doors open:
    - **dropping passes**: a tier leaves a pass out by not adding it, and culling removes
      whatever only that pass consumed;
    - **internal resolution**: the renderer creates transients at the internal size it chooses.
      If a dynamic-resolution governor makes the pool churn (rule 13), descriptions gain an
      allocated size separate from the used size, and passes render to a sub-rectangle. That is
      added with the governor, not before.

### Debugging and validation

20. **Validation** runs in Debug and Development builds and is compiled out of Shipping. Each
    error asserts with the graph, pass and resource names:
    - a read of a transient no earlier pass wrote;
    - a write to an import declared read-only;
    - a resource declared twice in one pass with conflicting uses (sampled and render target);
    - a use the resource's description or format forbids (for example storage on sRGB);
    - a handle from another graph or after reset;
    - an undeclared resource reached through `PassContext`.

    A transient that no pass uses, and a pass culled entirely, are logged at `Debug` level.
21. **Dumps.** A compiled graph prints as text (passes in order, culled ones marked, each
    resource's lifetime and pool slot, the barriers before each pass) and as Graphviz DOT. The
    text dump is the format of the compile tests' expected output.

### Testing

22. **Compile tests without a GPU** cover order, culling, barrier derivation (including
    `Undefined` transitions that name the previous use, across frames and across pool reuse),
    pool reuse within a frame, import state write-back and every validation error, by comparing
    text dumps. Execute tests on the null backend check recorded commands, group boundaries,
    serial and parallel recording giving the same commands, pool eviction and out-of-memory, under
    ThreadSanitizer in CI.
23. **GPU tests on both backends with validation on**, synchronization validation included, must
    run clean: a depth plus HDR color pass, a tonemap to the backbuffer, a history texture used
    for three frames, transient reuse within a frame, and a resize that evicts pool entries. The
    `scene_player` sample moves to the graph and drops its hand-written barriers; its pixel test
    stays as it is.
24. **Benchmarks** report compile and execute CPU time for a synthetic graph of 100 passes and
    200 resources. Initial target: compile under 0.1 ms on the Low desktop tier's CPU; a miss is
    reported, not a merge block, until measured on the reference machines.

### Out of scope for v1

25. Memory aliasing over heaps; async compute and copy queues; pass reordering; subresource
    tracking; split barriers; caching compiled graphs between frames; CPU upload management (the
    renderer imports its upload buffers); a struct-based pass parameter form; an immediate
    execution mode; a visual graph viewer in the editor (the DOT dump serves until the profiler
    UI, P9).

## Consequences

- Passes stop writing barriers; the RHI's barrier contract, including the `Undefined` rule, is
  implemented once and tested once without a GPU.
- Renderer code declares its frame in one place, and new passes add declarations instead of
  editing a shared struct of targets.
- Pooling without aliasing uses more memory than an aliasing allocator would; the Low tier's
  VRAM budget is the measurement that decides when aliasing is built.
- Declaration order means the author orders passes for overlap by hand until reordering or async
  compute is justified.
- Follow-up work: the `render_graph` module, its compile and null-backend tests, GPU tests on both
  backends, `scene_player` moved to the graph, and a section in the RHI guide (or a guide of its
  own). Later: an RHI heap and placed-resource ADR for aliasing, queues for async compute,
  timestamp queries for per-pass timing.
- **Cost:** about 2.5–3.5k lines including tests, size S–M (two to three weeks for one person),
  most of it in compile, the pool and the tests.
- **Revisit when:**
  - the P3 renderer's transient memory on the Low tier makes aliasing worth an RHI extension;
  - a profile shows async compute would hide a measurable amount of work;
  - compile time per frame exceeds its target, or the graph grows past a few hundred passes;
  - a pass needs per-mip or per-layer barriers (mip chain generation in one texture);
  - dynamic resolution makes the pool churn.
