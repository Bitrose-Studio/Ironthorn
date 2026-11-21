# RHI

The `rhi` module (`engine/ith/rhi`) is the engine's rendering hardware interface: the one API that
renderer passes, the UI renderer and the editor viewport are written against
([ADR-0009](../decisions/0009-rhi.md), amended by
[ADR-0021](../decisions/0021-rhi-device-structure-and-backend-modules.md)). It is a `client`
module in the `platform` layer ([ADR-0017](../decisions/0017-runtime-and-editor-separation.md));
a dedicated server doesn't link it. No graphics-API type appears in its headers.

What exists today is the backend-neutral part and the **null backend**, which has no GPU: it
keeps objects, handles, descriptor indices, frames and swapchains like a real backend and drops
the commands. It serves the tests, and the editor or player running without a GPU.

## A frame

```cpp
#include "ith/rhi/Device.h"

// Each backend has a factory: createNullDevice() in this module; createD3D12Device() and
// createVulkanDevice() in the backend modules, and a selection module that picks one at
// start-up (Direct3D 12 by default on Windows, otherwise Vulkan). The backends and the
// selection module don't exist yet.
ith::Expected<ith::rhi::Device, ith::rhi::Error> device = createDevice(...);
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
  every frame in an undefined layout and ends it in `Present`.
- **Frames.** At most `kFramesInFlight` (2) frames are in flight: `beginFrame()` waits for the
  frame two back. Command lists come from the frame's pool, may be recorded on several threads
  at once (one thread per list), and are submitted or discarded before `endFrame()`. A list
  opened outside a frame records nothing and its `submit()` fails.
- **Swapchains** are created from `ith::Window::nativeHandle()` with a positive size. After a
  resize, `resizeSwapchain()` records the new size and the next `beginFrame()` recreates the
  images under new handles; if that fails, it returns the error, the old images are stale, and
  the next `beginFrame()` tries again. A zero size, as for a minimized window, makes
  `beginFrame()` draw nothing. `Frame::backbuffer` is valid only until `endFrame()`.
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

## Not yet

- **GPU timing zones** (ADR-0009 rule 15) come with the profiler integration.
- **Bindless shader checks** (rule 16): the RHI rejects shaders with more than 128 bytes of
  constants, but can't yet see whether a shader declares resources outside the bindless model.
  That needs the shader metadata of ADR-0011 rule 6, which the shader pipeline doesn't produce
  yet.
- **Validation layers** (rule 18): `DeviceDesc::validation` and `gpuBasedValidation` do nothing
  until a GPU backend exists, and nothing yet enforces that shipping builds never load the
  layers.
- **Readback from textures**: there is no texture-to-buffer copy yet; it comes with the first
  backend, for image tests.
- **Mesh shaders and ray tracing** commands, with the renderer paths that use them.

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
