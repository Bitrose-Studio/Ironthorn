#pragma once

// GPU zones of the Direct3D 12 backend through Tracy's Direct3D 12 helpers (ADR-0007 rule 12,
// ADR-0009 rule 15; docs/development/rhi.md, "GPU zones"). Internal to the rhi_d3d12 module, and
// only in profiling builds (ITH_ENABLE_PROFILING). D3D12Profiler.cpp is the module's one source
// that includes Tracy (the tracy_confined test); no Tracy type appears here.
//
// One Tracy context times the graphics queue. Command lists open zones on it from any thread; the
// render thread collects the finished ones once per frame, in endFrame(). The context owns a
// timestamp query heap of 65,536 queries and a 512 KiB committed READBACK buffer that the zones
// resolve into, both created by Tracy outside D3D12MA, and 512 KiB of host memory.
//
// Zones use Tracy's own scope, which reports a zone ending on the thread that ends it. The device
// asserts (where asserts are compiled in) that no zone is open when a list is submitted or
// discarded, so an open zone is closed by another thread only when that rule was broken. Tracy
// collects zones in order and would wait for those of a list that never runs, so the profiler
// turns GPU zones off for the device when that happens (GpuProfiler::disable()).

#ifdef ITH_ENABLE_PROFILING

#include <directx/d3d12.h>

#include <memory>
#include <string_view>

namespace ith::rhi::d3d12 {

// The device's Tracy context for its graphics queue.
class GpuProfiler {
public:
    GpuProfiler();
    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;
    GpuProfiler(GpuProfiler&&) = delete;
    GpuProfiler& operator=(GpuProfiler&&) = delete;
    // Stops the profiler if it still runs.
    ~GpuProfiler();

    // Creates the context for `queue`, a direct queue of `device`. Leaves the profiler off, saying
    // why on stderr, when Tracy can't create it or the process already has too many Tracy GPU
    // contexts (Tracy numbers them with 8 bits; each backend takes at most 128).
    void start(ID3D12Device* device, ID3D12CommandQueue* queue);
    // Destroys the context: once the GPU is idle and before the device is released.
    void stop();
    // Collects the zones the GPU has finished; the render thread, once per frame.
    void collect();
    // Turns GPU zones off for the device for good, saying why on stderr the first time: a list
    // with zones never ran. Zones opened later record nothing. Thread-safe.
    void disable(std::string_view reason);
    // Whether disable() was called (tests).
    [[nodiscard]] bool disabled() const;

    // Whether zones are timed.
    [[nodiscard]] bool running() const { return m_context != nullptr; }

private:
    friend class GpuZones;
    struct Context;

    // Created by start() and destroyed by stop() on the render thread; command lists only read it
    // while the device exists.
    std::unique_ptr<Context> m_context;
};

// The open GPU zones of one command list: Tracy's scope objects, innermost last. Used by the
// thread recording the list.
class GpuZones {
public:
    GpuZones();
    GpuZones(const GpuZones&) = delete;
    GpuZones& operator=(const GpuZones&) = delete;
    GpuZones(GpuZones&&) = delete;
    GpuZones& operator=(GpuZones&&) = delete;
    // Precondition: no zone is open (endAll()), since ending one records into the command list.
    ~GpuZones();

    // Opens a zone in `list`, which is recording; a zone that records nothing when the profiler
    // doesn't run or is disabled, or no viewer is connected. Precondition: fewer than
    // kMaxGpuZoneDepth are open.
    void begin(GpuProfiler& profiler, ID3D12GraphicsCommandList* list, std::string_view name);
    // Closes the innermost zone. Precondition: one is open.
    void end();
    // Closes every open zone, innermost first, into the command list they were opened in, which
    // must still be recording.
    void endAll();
    // A new recording begins, which has opened no zone yet.
    void restart();
    // The current recording will never run: if it opened an active zone, the profiler is disabled.
    void abandoned(GpuProfiler& profiler);

private:
    struct Scopes;

    std::unique_ptr<Scopes> m_scopes;
};

} // namespace ith::rhi::d3d12

#endif
