# Render graph

The `render_graph` module (`engine/ith/render_graph`) schedules a frame's GPU passes
([ADR-0028](../decisions/0028-render-graph-v1.md)). Passes declare the resources they read and
write; the graph culls passes nobody needs, derives every barrier, takes transient resources
from a pool and records the passes into command lists, in parallel on the job system. It is a
`client` module in the `engine` layer, built on the [RHI](rhi.md) and the [job system](jobs.md);
the renderers and the editor viewport use it. Namespace `ith::rg`.

## A frame

```cpp
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"

ith::rg::RenderGraph graph({.name = "Frame"});      // long-lived; reset every frame
ith::rg::ResourcePool pool(device);                 // one per device, destroyed before it

// every frame, inside beginFrame()/endFrame():
graph.reset();
const rg::Texture backbuffer = graph.importBackbuffer(frame);
const rg::Texture depth = graph.createTexture(
    {.width = frame.width, .height = frame.height, .format = rhi::Format::D32Float, .debugName = "Depth"});
const rg::Texture hdr = graph.createTexture(
    {.width = frame.width, .height = frame.height, .format = rhi::Format::R16G16B16A16Float, .debugName = "HDR"});

graph.addPass("Scene", rg::PassType::Raster,
    [&](rg::PassBuilder& pass) {                       // setup: runs now, may capture by reference
        pass.renderTarget(hdr, rhi::LoadOp::Clear);
        pass.depth(depth, rhi::LoadOp::Clear);
    },
    [&scene](rg::PassContext& ctx) { scene.draw(ctx.list()); });
graph.addPass("Tonemap", rg::PassType::Raster,
    [&](rg::PassBuilder& pass) {
        pass.read(hdr, rg::TextureUse::Sampled, rhi::PipelineStage::PixelShader);
        pass.renderTarget(backbuffer, rhi::LoadOp::DontCare);
    },
    [hdr, pipeline](rg::PassContext& ctx) {            // execute: runs later, captures by value
        ctx.list().setPipeline(pipeline);
        ctx.list().pushConstants(TonemapConstants{.input = ctx.sampledIndex(hdr)});
        ctx.list().draw(3);
    });
graph.compile();
if (auto executed = graph.execute(pool, &jobs); !executed) {
    report(executed.error());                          // rhi.out_of_memory, rhi.device_lost, ...
}
```

A graph is declared, compiled and executed once per frame, then reset. All views of a frame
(the game, editor viewports) add their passes to the same graph.

## Passes

A pass has a name, a type, a setup callable and an execute callable.

| Type | Records | Shader uses default to |
|---|---|---|
| `Raster` | draws; the graph calls `beginRendering`/`endRendering` around the execute callable, with the viewport and scissor at the attachments' size | `VertexShader \| PixelShader` |
| `Compute` | dispatches | `ComputeShader` |
| `Copy` | copies; no shader uses, no attachments | — |

- **Setup** runs inside `addPass` and declares uses through `PassBuilder`: `read` and `write`
  for textures and buffers, `renderTarget` (in attachment order) and `depth` for raster
  attachments with their load operation and clear value. The optional `stages` argument narrows
  a shader use to some of the pass type's stages.
- **Execute** runs during `execute()`, possibly on a job system worker and in parallel with
  other passes. It must not write shared state. It reaches RHI objects only through
  `PassContext` (`texture()`, `buffer()`, `sampledIndex()`, `storageIndex()`,
  `shaderReadIndex()`, `shaderWriteIndex()`), which asserts that the pass declared the resource
  with a matching use. Execute callables live in the graph's arena and are destroyed at `reset()`.
- Each pass records inside a marker with its name, so PIX, RenderDoc and validation messages
  name it.
- `PassFlags::NeverCull` keeps a pass whose effect the graph can't see (a readback, a timestamp).

### Uses

| Use | Layout | Stages, accesses | RHI usage |
|---|---|---|---|
| `TextureUse::Sampled` | `ShaderRead` | shader stages, `ShaderRead` | `Sampled` |
| `TextureUse::Storage` | `ShaderWrite` | shader stages, `ShaderRead \| ShaderWrite` | `Storage` |
| `renderTarget()` | `RenderTarget` | `ColorOutput`, `RenderTargetWrite` (+ `RenderTargetRead` for `Load`) | `RenderTarget` |
| `depth()` | `DepthStencilWrite` | `DepthStencil`, `DepthStencilWrite` (+ `DepthStencilRead` for `Load`) | `DepthStencil` |
| `TextureUse::DepthSampled` | `DepthStencilRead` | shader stages, `ShaderRead` | `Sampled` |
| `CopySrc` / `CopyDst` | `CopySrc` / `CopyDst` | `Copy`, `CopyRead` / `CopyWrite` | `CopySrc` / `CopyDst` |
| `BufferUse::ShaderRead` | — | shader stages, `ShaderRead` | `ShaderRead` |
| `BufferUse::Storage` | — | shader stages, `ShaderRead \| ShaderWrite` | `ShaderWrite` |
| `BufferUse::Indirect` | — | `DrawIndirect`, `IndirectArgument` | `Indirect` |
| `BufferUse::Index` | — | `IndexInput`, `IndexBuffer` | `Index` |

A resource is used once per pass. Declaring it twice merges the declarations when they are the
same use, or when both are buffer reads (`Indirect` and `ShaderRead` of one buffer); anything
else is a conflict. Read-only depth attachments come when the Vulkan backend supports them.

## Resources

- **Transient** resources (`createTexture`, `createBuffer`) are described by size, format, mips,
  layers and a debug name. Their RHI usage is the union of the uses declared on them. They exist
  only in the frame's plan; at execute each takes a resource from the pool.
- **Imported** resources (`importTexture`, `importBuffer`) belong to the caller: the swapchain
  image, history textures, upload and readback buffers, asset textures. An import gives the RHI
  handle and its description (usage included), whether passes may write it, an optional final
  layout (which needs a writable import, since the transition writes the texture), and an
  `ImportedState*`.
- **`ImportedState`** holds a persistent resource's layout and the stages and accesses of its
  last use. The graph reads it at import and writes the final state back at execute, so next
  frame's first barrier orders after this frame's last use. Keep one per persistent resource,
  and reset it when you recreate the resource. It is read at import time, so a persistent
  resource is used by one graph per frame (all views share that graph). A failed `execute` writes
  nothing back.
- **`importBackbuffer(frame)`** imports the swapchain image: writable, from `Undefined` (acquiring
  it orders it after its presentation), ending in `Present`.

Handles (`rg::Texture`, `rg::Buffer`) are valid until the graph's `reset()`.

## Compiling

`compile()` turns the declarations into a plan and needs no device; the same declarations always
give the same plan.

1. **Order** is declaration order; passes are never reordered.
2. **Culling** walks back from the outputs: passes that write an import, and `NeverCull` passes.
   A pass is kept when one of its writes is needed by a kept later pass. A cleared or discarded
   attachment (`Clear`, `DontCare`) ends the need for earlier writes; reads, loads and partial
   writes (`Storage`, `CopyDst`) keep it. Transients only culled passes use are dropped.
   `RenderGraphDesc::culling = false` keeps every pass.
3. **Lifetimes and slots.** Each transient lives from its first to its last kept pass. At its
   first use it takes a pool slot: the first free slot of an earlier transient with the same
   description and usage, or a new one. Transients with disjoint lifetimes share a slot.
4. **Barriers** are derived per resource, for the whole resource, in one batch before each pass.
   A barrier comes before a use when the layout changes or either side writes; consecutive reads
   in one layout need none, and their stages are merged into the barrier before the first of
   them. A transient's first use, and a cleared or discarded attachment, transition from
   `Undefined` after the previous use's stages and, where it wrote, its write accesses, as
   [`Barrier.h`](rhi.md#rules) requires. The previous use of a slot's first transient is its
   holder in an earlier frame, which execute fills in from the pool.
5. **Store operations**: `DontCare` when nothing later needs the attachment's contents and it is
   not imported; `Store` otherwise.
6. **Final transitions** move imports with a final layout into it after the last pass
   (`Present` for the backbuffer).
7. **Groups**: the kept passes are cut into groups of `RenderGraphDesc::passesPerGroup`
   (default 8) consecutive passes, one command list each. Group boundaries depend on the plan
   only.

## Executing

`execute(pool, jobs)`, on the render thread inside an open frame:

1. takes a pooled resource for every slot, creating what the pool lacks; a creation failure
   (`rhi.out_of_memory`) fails `execute` before anything is recorded;
2. fills in the first barriers of pooled resources from their holders in earlier frames;
3. opens one command list per group (named after its first pass) and records the groups: in
   parallel on the foreground lane when `jobs` has foreground workers, otherwise in order on the
   calling thread. Serial and parallel recording record the same commands;
4. submits the lists in plan order with one `submit`, and records the final transitions at the
   end of the last group (a graph with no kept passes gets a list of its own for them);
5. remembers each pooled resource's last use, writes the imports' states back, and destroys
   pooled resources unused for `ResourcePoolDesc::maxUnusedFrames` (default 4) executes, through
   the RHI's deferred destruction.

A graph with validation errors fails `execute` with `rhi.invalid_usage` without recording.

**After a failure** nothing is written back: the imports' states and the pooled resources' last
uses stay as they were. A failure before the submission (out of memory, validation) leaves the
backbuffer outside `Present`; unless the device is lost, the caller executes a graph that only
imports the backbuffer before `endFrame()`, which moves it to `Present`, as `scene_player` does.
Adding a pass from inside another pass's setup callable is asserted, and that pass is not
added.

## The resource pool

`ResourcePool` keeps the transients' RHI resources between frames: buffers in GPU memory,
textures. A slot takes a resource whose description and usage match exactly (the debug name
aside) and that no other slot took this frame. There is no memory aliasing between different
descriptions in v1 (ADR-0028 rule 14): a resize creates new resources and the old ones go after
four unused frames. `textureCount()` and `bufferCount()` report what it holds, `clear()` destroys
everything. Destroy the pool before its device.

## Debugging

- **Dumps.** After `compile()`, `dump()` prints the plan: each pass with its group (or `culled`),
  its barriers and uses with load and store operations, the final transitions, and each
  resource's kind, size, format, usage, lifetime (`passes 1-3`) and slot. A barrier out of a
  pooled resource's earlier frame shows `(earlier frame)` until `execute()` fills it in.
  `dumpGraphviz()` gives the same graph in DOT for `dot -Tsvg`.

  ```text
  pass 3 'Tonemap' raster, group 0
    barrier texture 'Scene color': RenderTarget -> ShaderRead; (ColorOutput, RenderTargetWrite) -> (PixelShader, ShaderRead)
    barrier texture 'Backbuffer': Undefined -> RenderTarget; (None, None) -> (ColorOutput, RenderTargetWrite)
    use texture 'Scene color': Sampled
    use texture 'Backbuffer': RenderTarget, load DontCare, store Store
  ```

- **Validation** (Debug and development builds; compiled out of Shipping) asserts with the graph,
  pass and resource names, and makes `execute` fail:
  - a read (or load) of a transient that no earlier pass wrote;
  - a write to a read-only import, or a final layout on one;
  - `addPass` called from inside a setup callable;
  - one resource used twice in a pass with conflicting uses;
  - a use the format, description, import usage or pass type forbids (storage on sRGB, a render
    target with a depth format, an attachment outside a raster pass, a shader use in a copy pass);
  - a handle of another graph, from before a reset, or the null handle;
  - attachments of different sizes, or a raster pass without attachments;
  - through `PassContext`, a resource the pass didn't declare with a matching use.
- **Clobbering.** `RenderGraphDesc::clobberTransients` (Debug and development builds) copies
  every byte `0xFF` into each transient at its first use (NaN in float formats, white in UNORM
  ones), so a pass that reads stale pooled contents shows it. It adds `CopyDst` to the transients'
  usage. The copies cover each mip and layer of the color or depth plane: the stencil plane of
  `D32FloatS8Uint` keeps its contents, and block-compressed formats (none in the RHI yet) would
  need block row pitches the copies don't compute.

## Not yet

From ADR-0028's out-of-scope list: memory aliasing over heaps, async compute and copy queues, pass
reordering, per-mip and per-layer barriers, split barriers, caching compiled graphs, CPU upload
management, a struct-based pass parameter form, an immediate mode and a visual graph viewer.
Also not yet:

- per-pass GPU timings, which need RHI timestamp queries;
- the `Debug`-level log of unused transients and culled passes (ADR-0028 rule 20): the engine
  has no logging module yet, so the dump marks them (`unused`, `culled`) instead;
- the pass, use and barrier tables are vectors in the graph rather than in its arena (names and
  execute callables are in the arena); they keep their capacity across resets, so declaring and
  compiling a steady frame doesn't allocate, which `render_graph_allocation` checks. Execute
  allocates a little (the pool's vectors on growth, and the RHI's command lists).

## Tests and benchmark

- `tests/render_graph/` (CTest `render_graph`, no GPU, run under ASan and TSan in CI):
  - compile tests that compare text dumps: order, culling, store operations, slots,
    barrier merging, buffers, imports and groups;
  - execute tests on the null backend: groups and list names, serial against parallel recording
    (the same dump, the same lists per pass, the same command count), import write-back across
    frames, first barriers of pooled resources naming earlier frames, pool expiry,
    out-of-memory, clobbering and callable lifetimes;
  - validation tests with `AssertCapture`.
- `tests/render_graph/AllocationTests.cpp` (CTest `render_graph_allocation`, its own executable
  because it replaces the global allocation functions; not built in sanitizer builds) checks that
  declaring and compiling a steady frame of 22 passes allocates nothing.
- `tests/render_graph_gpu/` (CTest `render_graph_gpu`, label `gpu`, built with the tools for its
  Slang shaders) renders through the graph on Direct3D 12 (WARP, GPU-based validation) and Vulkan
  (synchronization validation; lavapipe in CI with `ITH_RHI_TEST_REQUIRE_VULKAN=1`), with the
  environment variables of the [RHI tests](rhi.md#testing-on-a-gpu): a depth and HDR color pass, a
  pass that loads both, a history texture accumulated over three frames, transient reuse within a
  frame, a tonemap into an imported target, a readback checked against the arithmetic, and a
  resize that evicts pool entries; once serially, once in parallel in groups of two with clobbered
  transients. A third case reuses a pooled texture within a frame through attachments and copies
  only (A cleared and copied out, then B in A's texture drawn with `DontCare` and copied out).

  What validation sees: synchronization validation checks attachment accesses, copies and layout
  transitions within one submission. It doesn't see shader reads and writes through bindless
  descriptors, nor hazards between submissions (frames), so a wrong barrier before a sampled read
  or before a pooled resource's first use after an earlier frame can pass it; the compile and
  execute tests' dumps check those barriers. The slot-reuse case is the one in-frame reuse the
  layer does check: breaking its barrier makes Direct3D 12's debug layer and Vulkan's
  synchronization validation both fail it.
- `ith_render_graph_benchmark` (CTest `render_graph_benchmark`, label `benchmark`) prints the
  median declare, compile and execute times of 100 compute passes and 201 resources on the null
  backend. It never fails on its numbers; ADR-0028's initial target is a compile under 0.1 ms on
  the Low desktop tier's CPU.

```powershell
./scripts/dev.ps1 build -Config Release -Target ith_render_graph_benchmark
ctest --test-dir build/windows -C Release -L benchmark -R render_graph --verbose
```
