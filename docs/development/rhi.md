# RHI

The `rhi` module (`engine/ith/rhi`) is the engine's rendering hardware interface: the one API that
renderer passes, the UI renderer and the editor viewport are written against
([ADR-0009](../decisions/0009-rhi.md), amended by
[ADR-0021](../decisions/0021-rhi-device-structure-and-backend-modules.md)). It is a `client`
module in the `platform` layer ([ADR-0017](../decisions/0017-runtime-and-editor-separation.md));
a dedicated server doesn't link it. No graphics-API type appears in its headers.

What exists today:

- the backend-neutral part (`rhi`);
- the **null backend** (`rhi`), which has no GPU: it keeps objects, handles, descriptor indices,
  frames and swapchains like a real backend and drops the commands. It serves the tests, and the
  editor or player running without a GPU;
- the **Direct3D 12 backend** (`rhi_d3d12`, Windows only; see [Direct3D 12](#direct3d-12));
- the **Vulkan backend** (`rhi_vulkan`, Windows and Linux; see [Vulkan](#vulkan));
- the **selection module** (`rhi_select`), whose `createDevice()` picks the backend.

## A frame

```cpp
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"

// Direct3D 12 by default on Windows, otherwise Vulkan (ADR-0021); a backend can be asked for.
ith::Expected<ith::rhi::Device, ith::rhi::Error> device = ith::rhi::createDevice({});
if (!device) {
    return fail(device.error()); // e.g. "The GPU 'X' lacks features Ironthorn needs: ... [rhi.unsupported_device]"
}
const ith::WindowSize size = window.pixelSize();
auto swapchain = device->createSwapchain({.window = window.nativeHandle(),
                                          .width = std::uint32_t(size.width),
                                          .height = std::uint32_t(size.height),
                                          .debugName = "Main window"});

while (running) {
    // on ith::EventType::WindowPixelSizeChanged:
    //     device->resizeSwapchain(*swapchain, event.width, event.height);
    auto frame = device->beginFrame(*swapchain);
    if (!frame) {
        return fatal(frame.error()); // the device is lost: log, report, shut down
    }
    if (!frame->has_value()) {
        continue; // minimized, or the swapchain is being recreated: nothing to draw
    }
    const ith::rhi::Frame& current = **frame;

    ith::rhi::CommandList& list = device->openCommandList("Main pass");
    list.barrier(ith::rhi::TextureBarrier{.texture = current.backbuffer,
                                          .stagesAfter = ith::rhi::PipelineStage::ColorOutput,
                                          .accessAfter = ith::rhi::Access::RenderTargetWrite,
                                          .layoutBefore = ith::rhi::TextureLayout::Undefined,
                                          .layoutAfter = ith::rhi::TextureLayout::RenderTarget});
    const std::array colors{ith::rhi::ColorAttachment{.texture = current.backbuffer}};
    list.beginRendering({.colors = colors});
    list.setPipeline(pipeline);
    list.pushConstants(DrawConstants{.vertexBuffer = device->shaderReadIndex(vertices)});
    list.draw(3);
    list.endRendering();
    list.barrier(ith::rhi::TextureBarrier{.texture = current.backbuffer,
                                          .stagesBefore = ith::rhi::PipelineStage::ColorOutput,
                                          .accessBefore = ith::rhi::Access::RenderTargetWrite,
                                          .layoutBefore = ith::rhi::TextureLayout::RenderTarget,
                                          .layoutAfter = ith::rhi::TextureLayout::Present});
    if (auto submitted = device->submit(list); !submitted) { ... }
    if (auto ended = device->endFrame(); !ended) { ... }
}
device->destroy(*swapchain); // before the window
```

## Rules

- **One backend per process.** `Capabilities::backend` says which one runs.
- **Handles, not pointers.** `BufferHandle`, `TextureHandle`, `SamplerHandle`, `PipelineHandle`
  and `SwapchainHandle` are 64-bit generational handles. `destroy()` makes a handle stale at once;
  using a stale handle is asserted in Debug and development builds, and `isValid()` tells.
- **Deferred destruction.** The object itself is released once the GPU has finished every
  submission that could use it *and* every command list open when it was destroyed has been
  submitted or discarded. Releases happen in `beginFrame()` and `waitIdle()`. Swapchains are the
  exception: `destroy()` waits for the GPU and releases them at once, so the window can go next.
- **Bindless.** Shaders reach buffers, textures and samplers by index:
  `shaderReadIndex()`, `shaderWriteIndex()`, `sampledIndex()`, `storageIndex()` and
  `samplerIndex()`. An index is fixed for the object's lifetime and reused only after the object
  was released. Per-draw data is at most 128 bytes of push constants
  (`CommandList::pushConstants`); larger data goes in a buffer whose index is in the constants.
- **Capacities and sizes** are fixed at device creation (`Capabilities::limits`): 262,144 resource
  descriptors, 2,048 samplers, per-type handle tables, and the largest buffer and texture.
  Running out of handles or descriptors fails with `rhi.out_of_handles` or
  `rhi.out_of_descriptors`; a resource over the size limits is a programmer error. The handle
  tables are allocated up front (about 6.5 MB with the default capacities), because other threads
  read them without a lock.
- **Explicit barriers.** The RHI tracks no resource state. A barrier names the stages and
  accesses before and after, and for textures the layouts (`Barrier.h`). A swapchain image starts
  every frame in an undefined layout and ends it in `Present`. A barrier out of `Undefined`
  discards the contents but still follows the texture's earlier use: name that use's stages, and
  its accesses where it wrote (a depth buffer reused every frame: `DepthStencil` and
  `DepthStencilWrite`).
  Vulkan's synchronization validation reports a write-after-write hazard without the access;
  Direct3D 12 drops it. The [render graph](render-graph.md) derives these barriers for its passes;
  code outside a graph, like the frame above, writes them by hand.
- **Frames.** At most `kFramesInFlight` (2) frames are in flight: `beginFrame()` waits for the
  frame two back. Command lists come from the frame's pool, may be recorded on several threads
  at once (one thread per list), and are submitted or discarded before `endFrame()`. A list
  opened outside a frame records nothing and its `submit()` fails.
- **Swapchains** are created from `ith::Window::nativeHandle()` with a positive size. After a
  resize, `resizeSwapchain()` records the new size and the next `beginFrame()` recreates the
  images under new handles; if that fails, it returns the error, the old images are stale, and
  the next `beginFrame()` tries again. A zero size, as for a minimized window, makes
  `beginFrame()` draw nothing. `Frame::backbuffer` is valid only until `endFrame()`.
- **Texture copies** move buffer data into or out of a box of one mip level and layer
  (`BufferTextureCopy::region`, a `TexelRegion`), with rows `kTextureCopyRowAlignment` (256)
  bytes aligned and the data at a `kTextureCopyOffsetAlignment` (512) offset, the stricter
  Direct3D 12 rules. A zero extent reaches to the end of the level, so the default region is the
  whole level; a font atlas updates only its changed rectangles. Depth formats copy whole levels
  only, as Direct3D 12 requires.
- **Mapped memory** of Upload and Readback buffers is persistent and host-coherent, and valid
  until the buffer's `destroy()`.
- **Errors** are `rhi::Error` with an `ErrorCode` and a message; `codeName()` gives the stable
  dotted code for logs. The code is an enum, not a string as in other modules, because callers
  switch on it: device loss shuts the engine down, out-of-memory makes streaming back off, the
  rest are bugs. Device loss and out-of-memory come from `submit()`, `endFrame()` and
  `beginFrame()`; after a loss every call that can fail reports `rhi.device_lost`, and the device
  can only be destroyed. Programmer errors (a description that breaks its rules, a call at the
  wrong time) assert and fail with `rhi.invalid_usage`; a shader with more than 128 bytes of
  constants fails with `rhi.invalid_shader`.
- **Recording checks.** Recording calls return nothing. In Debug and development builds the
  device checks each call (handles, usages, rendering state, bound pipeline, offsets and counts
  against buffer sizes, copy alignment) and asserts and drops a wrong one. Shipping builds skip
  the checks.
- **Debug names.** Every object and command list takes a non-empty debug name, passed to the API
  and tools.
- **Threads.** Creating and destroying buffers, textures, samplers and pipelines, the index and
  mapped-memory queries, `isValid()` and `openCommandList()` are thread-safe. Every use of a
  handle must happen before its `destroy()` on another thread; the caller orders them. Frames,
  submission, swapchains and `waitIdle()` belong to the render thread, the thread that created
  the device (asserted).

## Optional features and tiers

A device exists only if its GPU meets the minimum of
[ADR-0005](../decisions/0005-reference-hardware.md); otherwise creation fails with
`rhi.unsupported_device` and names every missing feature. Mesh shaders, hardware ray tracing,
variable-rate shading and sampler feedback are optional: `Capabilities` reports them, and the
renderer enables them per scalability tier. Nothing the Low tier needs may depend on them.
Mesh-shader pipelines and draws come with the renderer's mesh path (ADR-0021).

## GPU zones

A command list times its work for the profiler with GPU zones (ADR-0007 rule 12, ADR-0009
rule 15):

```cpp
list.beginGpuZone(pass.name); // any run-time string
...                           // the commands to time
list.endGpuZone();
```

The render graph wraps every pass in one, named after the pass. In the profiler the zones appear
on a GPU timeline named after the queue ("Vulkan graphics queue", "Direct3D 12 graphics queue"),
and each names the thread that recorded it ([Profiling](logging.md#gpu-zones)).

- **Only profiling builds record zones** (`IRONTHORN_PROFILING`, which defines
  `ITH_ENABLE_PROFILING`). Elsewhere `beginGpuZone()` and `endGpuZone()` are empty inline
  functions and compile to nothing: no check, no backend call.
- **Active zones.** By default the client collects on demand: a zone writes timestamps only
  while a viewer is connected, and otherwise costs one check. With
  `IRONTHORN_PROFILING_ON_DEMAND` off every zone is active from the start, two GPU timestamps and
  a copy of the name each, and the data waits in memory for a viewer; CI builds that way so that
  the zones' queries, resets and readbacks run under the validation layers
  ([Building with the profiler](logging.md#building-with-the-profiler)).
- **Rules:** zones nest at most `kMaxGpuZoneDepth` (16) deep; `endGpuZone()` closes an open
  zone; every zone is closed before its list is submitted or discarded. Profiling builds drop a
  call that breaks one of the first two in every configuration, and assert all three where
  asserts are compiled in (Debug and development builds); Release profiling builds don't check
  the third. A backend still closes a zone left open in a list, so that the profiler's timeline
  stays balanced.
- **Allowed:** zones may span `beginRendering()` and `endRendering()` and may open and close
  inside rendering. They are independent of markers.
- **Not checked:** zones that one thread opens in several lists must close in the reverse
  order, because the profiler nests each thread's GPU zones; and no zone may be open inside a
  multiview rendering pass, when the RHI gains them, since Vulkan timestamps then take one query
  per view. A zone shows as ending on the thread that ended it on Direct3D 12, and on the thread
  that began it on Vulkan; under the rules above they are the same thread.
- **One Tracy GPU context per device**, on its graphics queue. The backend creates it with the
  device and destroys it before the device, and collects the finished zones once per frame, in
  `endFrame()`; zones appear a frame or two after they ran, and those of the last frame or two
  before the device is destroyed are not collected.

The null backend ignores zones; they aren't counted as commands.

**Vulkan** (`VulkanProfiler.cpp`, Tracy's `TracyVulkan.hpp`). Tracy fetches its functions
through `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` (`TRACY_VK_USE_SYMBOL_TABLE`), since the
backend has no Vulkan symbols to link. The device takes one of two ways to put GPU timestamps on
the CPU's timeline, chosen when it is created (`VulkanGpuTiming.h`):

- **Calibrated**, where the GPU supports `VK_EXT_calibrated_timestamps`, reports the host clock
  Tracy reads (`QueryPerformanceCounter` on Windows, `CLOCK_MONOTONIC_RAW` on Linux) among its
  first four time domains, and has the `hostQueryReset` feature. The timelines are aligned again
  at every collection. Tracy looks for the extension's EXT functions only, so a driver with
  `VK_KHR_calibrated_timestamps` alone takes the other way.
- **Uncalibrated** otherwise: a timestamp written on the graphics queue while the device is
  created aligns the timelines once, so GPU zones may drift from the CPU's over a long session.
  The backend says so on stderr. Lavapipe takes this way (it reports `CLOCK_MONOTONIC`, not the
  raw clock), so CI runs it.
- **No GPU zones** when the graphics queue writes no timestamps (`timestampValidBits` 0), or when
  a start-up check fails that would otherwise leave Tracy waiting forever: creating a query pool
  of Tracy's size, a test submission to the queue (uncalibrated), or reading calibrated
  timestamps.

The extension and `hostQueryReset` are enabled only in profiling builds and only where the GPU
has them, so the minimum is the same in every build. The context's query pool holds 65,536
timestamps (512 KiB of GPU memory, outside VMA), with 1 MiB of host memory for the results.

The zones are the engine's own scopes on Tracy's context, not Tracy's `VkCtxScope`: they send
Tracy the same zone records, end a zone with the thread that began it, and know whether a list's
recording wrote queries. Collection runs in a small command buffer of the frame slot that the frame's
last batch in `endFrame()` submits: it reads the finished queries and resets them with `vkCmdResetQueryPool`, ordered after the commands that wrote them; a
reset from the host would race the GPU while their frame still runs.

**Direct3D 12** (`D3D12Profiler.cpp`, Tracy's `TracyD3D12.hpp`). The context times the direct
queue with a timestamp query heap of 65,536 queries and a 512 KiB committed `READBACK` buffer
that each zone resolves its two timestamps into as it ends, plus 512 KiB of host memory. Tracy
creates the heap and the buffer itself, outside D3D12MA, so they don't appear in D3D12MA's
statistics or budget; that is acceptable in builds that exist only for measuring. The backend
first checks that a heap of that size can be created. Collection reads the buffer and aligns the
clocks again (`GetClockCalibration`) every frame. The zones are Tracy's own `D3D12ZoneScope`,
whose heap, buffer and query numbering are private to it.

**Lists that never run.** Tracy collects a context's queries in order. A list with active zones
that never runs (discarded, closed unsubmitted by `endFrame()`, or in a batch that was skipped
or whose submission failed) leaves queries that will never be written, which Tracy would wait for
for good; on Vulkan the queries after them would also never be reset again, and once Tracy's pool
wrapped around, zones would write queries that aren't reset. So when the device reports such a
list abandoned, the backend turns GPU zones off for that device for good, and says so once on
stderr: later zones record nothing, and the zones of the dropped list and of the frames still in
flight are lost. They are also turned off when `endFrame()` finds a list still open (a broken
rule), and on Vulkan when ending or submitting its collection fails.
`vulkanGpuZonesDisabledForTesting()` and `d3d12GpuZonesDisabledForTesting()` report it.

Limits:

- Tracy numbers GPU contexts with 8 bits for the whole process and never reuses a number. Each
  backend takes at most 128, one per device; devices created after that have no GPU zones and
  say so on stderr. Only long test runs create that many devices.
- A list with active zones that never runs turns the device's GPU zones off (above). Destroying a
  Direct3D 12 device then waits up to 200 ms for the queries that never came.
- A viewer that disconnects loses the zones in flight; one that connects sees zones from then on.
- Not yet checked with a real Tracy viewer: how the zones display. The tests check the recording,
  the collection and the validation layers; zones were also made active by hand with a client
  that only completes the viewer's handshake.

Testing: `vulkanGpuTimingForTesting()` reports how a Vulkan device times its zones, and
`avoidCalibratedVulkanGpuTimingForTesting()` makes later devices take the uncalibrated way, so
that both run on one machine. The GPU suites record frames with zones (nested, around and inside
rendering, and in discarded lists or a failed submission followed by more than 65,536 queries'
worth) under the validation layers; `failNextVulkanSubmitForTesting(device, true)` makes a
submission fail without losing the device; the choice between the ways is tested without a GPU. They write timestamps only in a
build with `IRONTHORN_PROFILING_ON_DEMAND` off, as in CI.

## Not yet

- **Bindless shader checks** (rule 16): the RHI rejects shaders with more than 128 bytes of
  constants. Whether a shader declares resources outside the bindless model is checked by the
  backends (Direct3D 12 against its root signature, Vulkan by reading the SPIR-V's bindings), not
  yet from the shader metadata of ADR-0011 rule 6, which the shader pipeline doesn't produce yet.
- **Mesh shaders and ray tracing** commands, with the renderer paths that use them. Vulkan reports
  the capabilities but doesn't enable their extensions until then.
- **D3D12MA's and VMA's CPU allocations** go to the C runtime: their allocation callbacks aren't
  routed to an engine allocator yet (ADR-0009 rule 3), because the engine has no general-purpose
  allocator.
- **Read-only depth attachments on Vulkan**: `beginRendering()` uses a depth texture in the
  `DepthStencilWrite` layout.
- **`LoadOp::DontCare`** is treated as `Load` on Direct3D 12: the contents are kept, which the
  load operation allows. A render target or depth buffer whose barrier comes from `Undefined`
  is discarded by that barrier.

## Backends

`Device` and `CommandList` are concrete classes that do the backend-neutral work once: handle
tables, descriptor allocation, deferred destruction, frame pacing, swapchain image handles,
device-loss bookkeeping and the recording checks. Each calls its backend through an abstract
class with virtual functions (`detail::DeviceBackend` and `detail::CommandListBackend` in
`ith/rhi/Backend.h`), one call per RHI call. A backend keeps its native objects in arrays
indexed by the slot index of their handle, so the device never stores an API type. A backend
module hands its `DeviceBackend` to `detail::makeDevice()` from its factory function.
`Backend.h` lists the rules every backend follows (threads, timeline values, device loss,
swapchain recreation); renderer code never includes it.

The Direct3D 12 and Vulkan backends are separate modules, `rhi_d3d12` and `rhi_vulkan`, which
keep their vendor libraries private. The renderer and the editor depend on `rhi` only.
Applications call `createDevice()` (`ith/rhi_select/CreateDevice.h`), the one place that knows
the backends: Direct3D 12 by default on Windows, otherwise Vulkan, `Backend::Null` for no GPU,
and `rhi.backend_unavailable` for a backend this program doesn't have (Direct3D 12 outside
Windows) or that can't start. Vulkan can be chosen on Windows too.

## Direct3D 12

`rhi_d3d12` (`engine/ith/rhi_d3d12`) implements the backend on Direct3D 12 with the
[DirectX-Headers](https://github.com/microsoft/DirectX-Headers), the
[D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator) and
the Agility SDK, all at the versions in `cmake/IronthornDependencies.cmake`.
`createD3D12Device()` (`ith/rhi_d3d12/D3D12Device.h`) creates the device.

- **Adapters.** `AdapterPreference::HighPerformance` takes the first hardware adapter in DXGI's
  high-performance order that supports feature level 12_0; `Software` takes WARP.
- **Minimum features** (ADR-0005): Shader Model 6.6, Resource Binding Tier 3, wave intrinsics
  and enhanced barriers. A GPU without one fails with `rhi.unsupported_device`, naming each.
- **Binding.** One shader-visible CBV/SRV/UAV heap (`Limits::resourceDescriptors`, 262,144) and
  one sampler heap (2,048), and one root signature for every pipeline: 32 root constants (the
  128 bytes of `pushConstants()`) at register `b0, space0`, with the heaps directly indexed by
  SM 6.6 shaders (`ResourceDescriptorHeap[i]`, `SamplerDescriptorHeap[i]`). A shader declares its
  constants as a constant buffer at `b0, space0`; a pipeline whose shaders bind anything else
  fails with `rhi.invalid_shader`. Buffers are viewed as raw buffers (`ByteAddressBuffer`,
  `RWByteAddressBuffer`) of 4-byte words. A storage texture is viewed at mip 0, a sampled one
  with all its mips (the device refuses sRGB storage textures on every backend).
- **Memory.** D3D12MA allocates every buffer and texture. Upload and Readback buffers stay
  mapped.
- **Barriers** map one to one onto enhanced barriers. A shader read (`Access::ShaderRead`) maps to
  a shader-resource read, or to an unordered-access read when the texture layout is
  `ShaderWrite` (or a `General` storage texture). A texture barrier out of `Undefined` keeps
  its stages but drops its access before, which Direct3D 12 refuses there; Vulkan needs it, so
  name the earlier writes anyway (a depth buffer reused every frame: `DepthStencil` and
  `DepthStencilWrite` before).
- **Commands.** One direct queue and one fence carry the timeline. Each pooled command list has
  its own allocator, reset when the list is begun. `beginRendering()` sets render targets and
  clears; indirect draws and dispatches use command signatures; markers are PIX-readable events.
- **Shaders** must be DXIL in a DXBC container; anything else fails with `rhi.invalid_shader`.
- **Swapchains** are flip-model DXGI swapchains with three buffers, created from the window's
  `HWND`. For an sRGB format the buffers are UNORM and their render-target views sRGB. Tearing
  is used when `vsync` is off and the display allows it.
- **Device loss.** DRED's breadcrumbs and page-fault reports are on in every build; a lost
  device's error names the removal reason, the command lists the GPU didn't finish and the
  faulting allocation. If signaling the timeline fails after work was queued, the backend removes
  the device, so that nothing is left untracked, and reports the loss. Waits for the GPU look for
  device removal every second, so none waits without a limit. For tests,
  `removeD3D12DeviceForTesting()` removes a device and `failNextD3D12SignalForTesting()` makes the
  next signal fail. The debug layer, DRED and a removal affect every device of the process on the
  same adapter: create a process's devices with the same validation setting.

### The Agility SDK

The backend is built against the Agility SDK's headers (DirectX-Headers of the same version),
so a process must run on the Agility SDK's `D3D12Core.dll` rather than the older one in Windows.
The executable does that by exporting `D3D12SDKVersion` and `D3D12SDKPath`, and by having the
runtime in a `D3D12` folder next to it:

    <executable directory>/
        game.exe            exports D3D12SDKVersion = 620, D3D12SDKPath = ".\D3D12\"
        D3D12/
            D3D12Core.dll
            d3d12SDKLayers.dll   (not in Shipping)

`ith_add_application` sets this up on Windows for every player and editor application whose
`MODULES` include `rhi_select`, through
`ith_use_d3d12_agility_sdk(<target>)` (`cmake/IronthornDependencies.cmake`). Other executables
that create a Direct3D 12 device, like test suites, call that function themselves. When Windows
has a newer runtime than the package, it uses its own. When the process runs an older one (the
exports or the folder are missing), `createD3D12Device()` fails with `rhi.backend_unavailable`.

**Validation** (`DeviceDesc::validation`, on by default in Debug) needs the debug layer, which
needs both the package's `d3d12SDKLayers.dll` and the Windows optional feature **Graphics
Tools** (Settings > System > Optional features, or
`Add-WindowsCapability -Online -Name Tools.Graphics.DirectX~~~~0.0.1.0` as administrator).
Without it, a device that asks for validation fails with `rhi.backend_unavailable`. The debug
layer's warnings and errors go to stderr; errors and corruption are asserted.

## Vulkan

`rhi_vulkan` (`engine/ith/rhi_vulkan`) implements the backend on Vulkan 1.3 with the C API of
[Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) and the
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), at the
versions in `cmake/IronthornDependencies.cmake`. `createVulkanDevice()`
(`ith/rhi_vulkan/VulkanDevice.h`) creates the device, on Windows and Linux.

- **Loader.** Nothing links Vulkan: the backend opens the system's loader (`vulkan-1.dll`,
  `libvulkan.so.1`) when a device is created and fetches the functions it calls through
  `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` (`VulkanLoader.h`). Without the loader, or
  without a driver, `createVulkanDevice()` fails with `rhi.backend_unavailable`.
- **Adapters.** `AdapterPreference::HighPerformance` takes the first device, discrete GPUs before
  integrated and virtual ones, that meets the minimum; `Software` takes a CPU implementation
  such as lavapipe.
- **Minimum features** (ADR-0005, amended by
  [ADR-0022](../decisions/0022-gpu-minimum-features-completed.md)): Vulkan 1.3, dynamic rendering,
  synchronization2, maintenance4, timeline semaphores, buffer device address, `drawIndirectCount`,
  multi-draw indirect, independent blending, cube map arrays, robust buffer access (also with
  update-after-bind descriptors), subgroup ballot and arithmetic in compute, and descriptor
  indexing (runtime arrays, partially bound and update-after-bind descriptors, non-uniform
  indexing) with 262,144 resource descriptors and 2,048 samplers per stage and in all pools. A
  device without one fails with `rhi.unsupported_device`, naming each.
- **Out-of-bounds buffer accesses** stay inside the buffer (robust buffer access): they can't
  touch other memory, but a read past the end may return zero or another value from the same
  buffer, and a write may land elsewhere in it. Shaders must not rely on the value.
- **Binding.** One descriptor set, at set 0, bound to every command list. Its runtime arrays share
  the device's bindless indices, as Direct3D 12's single heap does:

  | Binding | Type | Holds | Slang |
  |---|---|---|---|
  | 0 | storage buffers | read and write indices of buffers | `[[vk::binding(0, 0)]] RWByteAddressBuffer buffers[];` (or `ByteAddressBuffer`) |
  | 1 | sampled images | sampled indices | `[[vk::binding(1, 0)]] Texture2D textures[];` |
  | 2 | storage images | storage indices | `[[vk::binding(2, 0)]] RWTexture2D<float4> images[];` |
  | 3 | samplers | sampler indices | `[[vk::binding(3, 0)]] SamplerState samplers[];` |

  One pipeline layout serves every pipeline: the set and 128 bytes of push constants for every
  stage. A shader declares its constants as `[[vk::push_constant]] ConstantBuffer<T>` (the
  attribute is ignored for DXIL, where the same block is the constant buffer at `b0, space0`);
  both APIs see it at offset 0. A shader that binds anything else (another set or binding, or a
  uniform buffer) fails with `rhi.invalid_shader`. Sampled descriptors are written for
  `ShaderRead` (and `DepthStencilRead`) layouts, storage descriptors for `General` and
  `ShaderWrite`. Read and write indices of a buffer are both storage-buffer descriptors: Vulkan
  has no read-only descriptor of its own, so a shader that declares a read index as
  `RWByteAddressBuffer` can write through it. Declare read indices as `ByteAddressBuffer`.
- **Memory.** VMA allocates every buffer and texture. Upload and Readback buffers are host-coherent
  and stay mapped. Every command list ends with a barrier that makes its writes visible to the
  CPU, so a Readback buffer can be read once the frame that wrote it is done.
- **Barriers** map one to one onto synchronization2. `RenderTarget` and `DepthStencilWrite` are
  `VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL`, `ShaderRead` and `DepthStencilRead`
  `VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL`, `General` and `ShaderWrite` `VK_IMAGE_LAYOUT_GENERAL`.
- **Commands.** One graphics queue and one timeline semaphore carry the timeline. Each pooled
  command list has its own command pool, reset when the list is begun. The viewport's height is
  negative, so clip space is the same as on Direct3D 12 (ADR-0006). Markers are debug-utils
  labels.
- **Shaders** must be SPIR-V; anything else fails with `rhi.invalid_shader`. The backend takes
  trusted SPIR-V from the engine's shader compiler, which validates it, and runs no SPIR-V
  validator itself: it only reads the module's resource variables (`SpirvCheck.h`) and refuses
  any outside the bindless set, by set, binding, type and array length. A fuzz target
  (`ith_rhi_vulkan_spirv_fuzz`, Clang sanitizer builds) runs that reader on arbitrary bytes. The shader compiler
  keeps entry-point names in SPIR-V, so `ShaderDesc::entryPoint` names the same function on both
  APIs.
- **Swapchains** are created from Win32, X11 (Xlib) and Wayland windows, with three images where the
  surface allows. Where the window system fixes the image size (Win32, X11), the images take the
  window's size, so pass the window's pixel size to `resizeSwapchain()`. Without `vsync` the
  swapchain presents immediately or in mailbox mode where the surface allows. An acquired image
  is waited for by a small submission that signals a second timeline semaphore, which the frame's
  later submissions wait for.
- **Device loss.** `VK_ERROR_DEVICE_LOST` from any call makes the device lost; where the driver has
  `VK_EXT_device_fault`, the error carries its description, faulting addresses and vendor
  reports. If signaling the timeline at the end of a frame fails, the device is treated as lost,
  as on Direct3D 12. Waits for the GPU run in steps of a second that look for loss; acquiring a
  swapchain image gives up with `rhi.backend_failure` after ten of them. Waits for an idle queue
  (before a swapchain is recreated or destroyed, and after a loss) have no limit: Vulkan has no
  bounded form of them, and a hung GPU ends them as a device loss. A failed presentation makes the
  next `beginFrame()` recreate the swapchain. For
  tests, `failNextVulkanSubmitForTesting()` makes the next queue submission fail with
  `VK_ERROR_DEVICE_LOST`.
- **Validation** (`DeviceDesc::validation`, on by default in Debug) enables the Khronos validation
  layer when it is installed, with synchronization validation turned on through
  `VK_EXT_layer_settings`. Its warnings and errors go to stderr with their message id (a VUID, or
  `SYNC-HAZARD-...`); validation errors are asserted. Without the layer the device runs without
  validation and says so on stderr. Shipping builds never load the layer. The layer comes with the
  [Vulkan SDK](https://vulkan.lunarg.com/sdk/home).

## Testing on a GPU

### Direct3D 12

`tests/rhi_d3d12` (CTest name `rhi_d3d12`, label `gpu`, Windows only) runs devices, resources,
deferred destruction, clears read back from a render target, discarded render targets (also
under GPU-based validation when validation is on), copy round trips through buffers and through
textures at deeper mips, layers, 3D slices and both depth formats, a partial upload and readback of
a rectangle, lists recorded on several threads while others create and destroy resources, a
swapchain on a hidden window, device loss and failed timeline signals and, when the tools are
built, compute and draw pipelines compiled from Slang at run time. It uses WARP, so it runs on
hosted CI runners, with validation on. Two environment variables change that:

- `ITH_RHI_TEST_ADAPTER=hardware` runs it on the fastest hardware GPU;
- `ITH_RHI_TEST_VALIDATION=off` runs it without the debug layer, on a machine without Graphics
  Tools.

### Vulkan

`tests/rhi_vulkan` (CTest name `rhi_vulkan`, label `gpu`, Windows and Linux) runs the same
cases on Vulkan, with the barriers synchronization validation asks for, plus a check that
synchronization validation reports a missing barrier, simulated device loss in `submit()` and at
the end of a frame, and a pipeline refused for binding outside the bindless set. The swapchain
case runs on Windows only. It picks the fastest hardware GPU, or a software one (lavapipe) when
there is none, with validation on where the layer is installed. Environment variables:

- `ITH_RHI_TEST_ADAPTER=hardware` or `=software` uses only that kind of device;
- `ITH_RHI_TEST_VALIDATION=off` runs it without the validation layer;
- `ITH_RHI_TEST_REQUIRE_VULKAN=1` turns "no Vulkan loader, driver or device" from a skip into a
  failure, and requires the validation layer (unless validation is off), for machines that must
  run the suite.

### Backend selection

`tests/rhi_select` checks backend selection on every platform; its Vulkan case accepts a device or
`rhi.backend_unavailable`/`rhi.unsupported_device`.

### Scenes

`tests/scene_player` (CTest name `scene_player`, label `gpu`, built with the tools) renders the
scenes of the `scene_player` sample offscreen, through a render graph like the sample, on each
backend the machine has, with the same
environment variables as above, and checks the pixels against the geometry, without reference
images: the triangle's corners, centroid, vertex colors and covered area; a near cube hiding a
larger far one drawn after it (reversed depth); and the 10,000-cube field's sky and its nearest
cubes' colors. Direct3D 12 runs with GPU-based validation (what validation can see: [Samples](#samples)). Where both backends run, it also
checks that they render the same images (at most 1% of texels differ by more than 2 levels, 5%
for the field).

## Testing on the null backend

`createNullDevice()` (`ith/rhi/NullDevice.h`) takes the capabilities and limits to report and
an optional `NullGpu`, which simulates the GPU's progress and injects faults:

```cpp
ith::rhi::NullGpu gpu;
gpu.setAutoComplete(false);          // work completes only when the test says so
auto device = ith::rhi::createNullDevice({.gpu = &gpu});
...
gpu.complete(gpu.signaledValue());   // the GPU catches up
gpu.loseDevice("simulated hang");    // the next submit, wait or present reports rhi.device_lost
gpu.failNextCreate();                // the next create call fails with rhi.out_of_memory
gpu.failNextResize();                // the next swapchain recreation fails
```

`NullGpu` also counts live backend objects, recorded commands, submissions, presents and
swapchain builds, so tests check behaviour without a GPU. The tests in `tests/rhi` cover handle
lifetimes, deferred destruction, frame pacing, swapchains, device loss, the recording checks,
and the thread-safe calls racing the render thread (run under ThreadSanitizer in CI).

## Samples

`samples/scene_player` draws with the bindless model end to end: shaders read vertices and
instances from buffers whose `shaderReadIndex()` is in the push constants, and pipelines have no
vertex input layout. `--scene triangle` draws three vertices in one `draw(3)`; the default scene
draws 10,000 cubes in one `drawIndexed(36, 10000)` with a 16-bit index buffer, per-instance
position, size and color in a storage buffer, a camera in the push constants, a reversed `D32Float`
depth buffer and one directional light. `--no-vsync` makes the printed frame time the time a
frame takes. In a profiling build, the GPU time of its render-graph pass shows in the profiler
as a [GPU zone](#gpu-zones).

The scenes' code (`Scenes.h`, `Scenes.cpp`) is compiled into the sample and into its GPU test.
The sample and the test draw each scene in one pass of a [render graph](render-graph.md), which
imports the swapchain image (or the test's target), takes the depth buffer from its pool and
derives every barrier, including the one that discards the reused depth buffer each frame after
the earlier frames' depth writes (see [the rule for barriers out of `Undefined`](#rules)). Each frame
is its own submission, and synchronization validation checks attachments, copies and layout
transitions within a submission only, not bindless shader reads nor hazards between submissions:
the barriers across frames are checked by the render graph's own tests. The Slang shaders (`Triangle.slang`,
`Cubes.slang`) are compiled at build time by the pinned `slangc` with `ith_embed_shaders()` and
embedded in each program, as
[ADR-0023](../decisions/0023-build-time-shaders-for-samples-and-tests.md) (embedded shaders are
compiled at build time with slangc) allows for samples and tests; so the sample and the test exist
only with `IRONTHORN_BUILD_TOOLS`. One source serves both APIs through `ITH_SPIRV`, with the
engine's compile options ([shaders](shaders.md)). Games load cooked shaders instead (ADR-0011
rule 9).
