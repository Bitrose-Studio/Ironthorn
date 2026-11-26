# ADR-0044: GPU profiling zones (amends 0007, 0009)

- **Status:** Accepted
- **Date:** 2026-11-26
- **Related:** completes [ADR-0007](0007-logging-and-profiling.md) rule 12 ("the exact API belongs
  to the RHI ADR") and [ADR-0009](0009-rhi.md) rule 15 (a scoped GPU zone on command lists);
  [ADR-0028](0028-render-graph-v1.md) rule 18 (per-pass GPU timing);
  [ADR-0001](0001-cpp-language-and-safety-rules.md) (asserts, no exceptions),
  [ADR-0004](0004-modules-and-layering.md) (no third-party types in public headers),
  [ADR-0021](0021-rhi-device-structure-and-backend-modules.md) (backend modules),
  [ADR-0022](0022-gpu-minimum-features-completed.md) (minimum GPU features),
  [RHI guide](../development/rhi.md#gpu-zones)

## Context

ADR-0007 and ADR-0009 promise GPU zones on command lists, implemented with Tracy 0.15.0's Vulkan
and Direct3D 12 helpers in private backend sources, and leave the API open. The first
implementation settled the API and found several ways Tracy's helpers, used as documented, fail
inside this RHI:

- Tracy's scope objects are RAII types; they can't appear in a public RHI header, and the render
  graph opens and closes a pass's zone in one function, like its markers.
- Tracy's scopes report a zone ending on the thread that ends it, while a list may be closed or
  reused by another thread than the one that recorded it.
- Tracy collects a context's queries in order. A list with zones that never runs (discarded, or
  its batch skipped or failed) leaves queries that are never written: Tracy waits for them for
  good on Vulkan, where the queries after them are also never reset again, and on Direct3D 12
  until 32,768 later zones are pending.
- Resetting Vulkan queries from the host, as Tracy's calibrated path does, races command buffers
  the GPU is still running.
- Some start-up failures make Tracy retry or wait forever instead of failing.
- Vulkan calibrated timestamps need an extension, a host time domain Tracy reads and the
  `hostQueryReset` feature, none of which are in the minimum spec.

## Options considered

The API:

- **`beginGpuZone(name)` / `endGpuZone()` on `rhi::CommandList` (chosen)**, the same shape as
  `beginMarker`/`endMarker`. No Tracy type in the header; the render graph calls them in one
  function.
- **An RAII zone object.** Exception-free and hard to leave open, but it would wrap a Tracy scope
  (or a pimpl of one) in a public header, and the graph's begin and end sit in one loop body
  around branches, where a scope object adds nothing.
- **Fold zones into markers.** One call fewer, but markers exist in every build for debuggers and
  validation, while zones exist only in profiling builds; a marker inside rendering would also
  have to obey zone rules.

The zone scopes:

- **Tracy's scopes (`VkCtxScope`, `D3D12ZoneScope`) on both backends.** Simplest, but a zone
  closed by another thread is reported on that thread.
- **Engine-owned scopes on both.** Control over the thread an end is reported on, but Tracy's
  Direct3D 12 context keeps its query heap, readback buffer and query numbering private to its
  scope class; owning the scope there means patching or forking Tracy.
- **Engine-owned on Vulkan, Tracy's on Direct3D 12 (chosen).** Vulkan's context exposes what an
  engine scope needs (`NextQueryId`, `GetQueryPool`, `GetId`). On Direct3D 12 the rule that no
  zone is open when a list is submitted or discarded keeps ends on the recording thread, where it
  is asserted. The price is a dependency on Tracy's item layout on Vulkan.
- **Our own GPU timer with no Tracy.** Would have to rebuild Tracy's GPU timeline and viewer; the
  in-editor profiler (P9) is the place for that, not now.

Lists that never run:

- **Leave it to Tracy.** The GPU timeline stalls; on Vulkan, once the query counter wraps, zones
  write queries that were never reset (VUID-vkCmdWriteTimestamp-None-00830).
- **Retire the lost query ids at a later collection.** Tried first. Each fix left ids stranded on
  another reachable path (a submit failure that doesn't lose the device, list indices that are
  not reused, a viewer connecting between two reads of the connection state, a failed collection
  buffer), and each patch added machinery.
- **Turn the device's GPU zones off for good (chosen).** No new queries are issued, so the Vulkan
  counter can't wrap onto unwritten queries. The cost: zones are lost after an event that normal
  frames never cause.

Vulkan timing:

- **Calibrated only.** Needs an extension and a feature outside the minimum spec, so lavapipe
  and some drivers would get no GPU zones at all.
- **Calibrated where fully supported, uncalibrated otherwise (chosen).** Zones everywhere a queue
  writes timestamps; uncalibrated timelines may drift over a long session, and the backend says
  so on stderr.

## Decision

### API

1. **`rhi::CommandList::beginGpuZone(std::string_view name)` and `endGpuZone()`** open and close
   a GPU timing zone. `name` is any run-time string, copied when needed. There is no RAII zone
   type.
2. **Without `ITH_ENABLE_PROFILING` both functions are empty inline functions** and compile to
   nothing. The define is `PUBLIC` on `foundation`, so every translation unit agrees on it, and
   `CommandList`'s layout is the same in both builds.
3. **Three rules, checked in profiling builds:** zones nest at most `kMaxGpuZoneDepth` (16) deep
   per list; `endGpuZone()` closes an open zone; every zone is closed before its list is
   submitted or discarded (`Device::submit()`, `Device::discard()`). A call that breaks one of
   the first two is dropped in every profiling build; all three are asserted where asserts are
   compiled in. Release profiling builds don't check the third. Zones may span
   `beginRendering()` and `endRendering()` and may open and close inside rendering. They are
   independent of markers.
4. **Zones from two lists recorded on one thread nest LIFO**: a thread closes its zones in the
   reverse order it opened them, across lists. Not checked.
5. **No zone may be open inside a multiview render pass.** Vulkan timestamps there take one query
   per view; the RHI has no multiview yet, and adding it revisits this rule.

### Render graph

6. **Every pass records inside a GPU zone named after the pass**, opened just inside its marker
   and closed just before the marker ends, so the zone covers the pass's barriers and commands.
   The bottleneck classifier and the Scalability Lab will read their own timings when they are
   built.

### Contexts

7. **One Tracy GPU context per device, on its graphics queue.** It is created with the device,
   collected once per frame in `endFrame()` on the render thread, and destroyed after the device
   is idle and before the device is destroyed.
8. **At most 128 contexts per backend per process.** Tracy numbers GPU contexts with 8 bits and
   never reuses a number; 128 per backend keeps two backends within 256. Later devices have no GPU
   zones and say so on stderr.

### Vulkan

9. **Calibrated** (`TracyVkContextHostCalibrated`) when the device has
   `VK_EXT_calibrated_timestamps`, reports the host clock Tracy reads (`QueryPerformanceCounter`
   on Windows, `CLOCK_MONOTONIC_RAW` on Linux) among the first four time domains (all Tracy
   reads), and has `hostQueryReset`. If the start-up calibration call then fails, the device has
   no GPU zones; it does not fall back. **Uncalibrated** otherwise: `TracyVkContextCalibrated`
   with a `vkGetDeviceProcAddr` wrapper that returns null for `vkGetCalibratedTimestampsEXT` and
   `vkGetCalibratedTimestampsKHR`, so Tracy aligns the timelines once at start-up whatever the
   driver returns. **No GPU zones** when the graphics queue writes no timestamps.
10. **Profiling builds enable the extension and `hostQueryReset` where the GPU has them** (the
    calibrated path only), so device creation differs between profiling and normal builds. The
    minimum spec (ADR-0005, ADR-0022) is unchanged: neither is required.
11. **Query resets run on the GPU**: collection records them into a command buffer of the frame
    slot, submitted with the frame's last batch in `endFrame()`, ordered after the commands that
    wrote the queries. Host resets are not used after start-up.

### Zone scopes

12. **Vulkan owns its zone scopes** and writes Tracy 0.15.0's GPU-zone protocol items itself. A
    zone's end carries the handle of the thread that began it. A Tracy upgrade must re-check
    those items against the new version.
13. **Direct3D 12 uses Tracy's `D3D12ZoneScope`**, because its context's internals are private.
    A zone is reported ending on the thread that ends it; rule 3 keeps that the recording thread
    where it is asserted.
14. **A list that never runs turns GPU zones off for its device, for the device's lifetime.** The
    first time a list whose recording wrote zone queries stops recording without running
    (discarded, closed unsubmitted by `endFrame()`, or in a batch that was skipped or failed to
    submit), the device tells the backend through `CommandListBackend::abandoned()`, in profiling
    builds, once the outcome is known. The backend then opens no more zones and says so once on
    stderr. Vulkan does the same when its collection buffer can't be ended or submitted. Zones
    already open still write their end query, so queries stay paired; the zones of the dropped
    list and of frames in flight are lost.
15. **A backend closes zones left open** before it ends or resets a list's recording, and before
    it destroys the list, so the timeline stays balanced even after an assert.

### Start-up guards

16. **Everything Tracy would retry or wait on is checked first.** On failure the device runs
    without GPU zones and says why on stderr.
    - A query pool (Vulkan) or timestamp heap (Direct3D 12) of 65,536 queries, Tracy's size, is
      created and destroyed first; Tracy halves the size and retries forever on failure.
    - Calibrated Vulkan: one `vkGetCalibratedTimestampsEXT` call must succeed.
    - Uncalibrated Vulkan: an empty submission must finish within a 5 s fence wait. After a
      timeout the backend waits for the queue to go idle without a limit, because the pending
      fence can't be destroyed earlier.
    - A context without a valid Tracy context id gets no zones.

### Memory

17. **Tracy's resources live outside the engine's allocators**: the Vulkan query pool (65,536
    timestamps) and the Direct3D 12 timestamp heap and its readback buffer (512 KiB) are not in
    VMA or D3D12MA budgets or statistics. Acceptable because only profiling builds have them.

### Testing

18. **`IRONTHORN_PROFILING_ON_DEMAND`** (default `ON`) keeps Tracy's on-demand mode. `OFF` makes
    zones record without a connected viewer, keeping all profiling data in memory until one
    connects. It is meant for test executables.
19. **CI runs only the GPU suites with it `OFF`**, under the validation and debug layers: Linux
    on lavapipe (the uncalibrated path), Windows on WARP. The default on-demand profile build
    runs the full suite. Checking zones in a real Tracy viewer stays a manual step.

## Consequences

- Pass timings appear on the profiler's GPU timeline under each pass's name, at no cost in
  builds without profiling.
- The RHI API carries no Tracy type; the backends' profiler sources hold the only Tracy includes
  outside `Profile.h`, enforced by the `tracy_confined` lint.
- Owning the Vulkan zone scopes costs a dependency on Tracy's protocol items: every Tracy upgrade
  re-checks them, beyond the viewer-version check of ADR-0007 rule 14.
- One discarded list with active zones, or one failed submission, ends GPU profiling on that
  device until it is recreated. Normal frames don't do either. The stderr line says why.
- Profiling builds create Vulkan devices with an extra extension and feature where available, so
  a driver bug in those paths could show only in profiling builds.
- The calibrated Vulkan path is not exercised in CI (lavapipe takes the uncalibrated one); it is
  covered by developer machines and the manual viewer check.
- ADR-0007 and ADR-0009 are marked as amended by this ADR in their headers and in the index.
- **Revisit when:**
  - Tracy is upgraded (protocol items, context numbering, query pool sizes, the private
    Direct3D 12 internals);
  - lists with zones are routinely discarded (for example by editor workflows), so that losing
    zones costs more than the machinery to retire query ids;
  - the RHI gains multiview (rule 5);
  - async compute or copy queues are added (one context per queue, collection per queue);
  - the in-editor profiler (P9) needs the same timings without Tracy.
