#include "ith/rhi_d3d12/D3D12Profiler.h"

#ifdef ITH_ENABLE_PROFILING

#include "ith/foundation/Assert.h"
#include "ith/rhi/CommandList.h"

// Tracy's Direct3D 12 helpers include the SDK's <d3d12.h>, which shares its include guard with
// DirectX-Headers' copy: it must come after <directx/d3d12.h> (D3D12Profiler.h), so that the
// backend's headers are the ones in use.
#include <directx/d3d12.h>
#include <tracy/TracyD3D12.hpp>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>

namespace ith::rhi::d3d12 {
namespace {

// Tracy numbers GPU contexts with 8 bits for the whole process (NextGpuContextId) and never reuses
// a number. Each backend takes at most 128, so that the two together stay within Tracy's 256; a
// process that creates more devices (a long test run) gets no GPU zones on the later ones.
constexpr std::uint32_t kMaxContexts = 128;
std::atomic<std::uint32_t> g_contexts{0};

constexpr std::string_view kContextName = "Direct3D 12 graphics queue";
// The size of Tracy's timestamp query heap (D3D12QueueCtx::m_queryLimit in 0.15.0). Tracy halves
// it while creating the heap fails, forever if it always fails, so start() creates one first.
constexpr UINT kQueryCount = 64 * 1024;
// Tracy copies a zone's name into a 16-bit-sized source location; longer names are cut.
constexpr std::size_t kMaxNameSize = 1024;

struct TracyContextDeleter {
    void operator()(tracy::D3D12QueueCtx* context) const { TracyD3D12Destroy(context); }
};

} // namespace

struct GpuProfiler::Context {
    std::unique_ptr<tracy::D3D12QueueCtx, TracyContextDeleter> tracy;
    // Set for good once a list with zones never ran (GpuProfiler::disable()); read by every thread
    // that opens a zone.
    std::atomic<bool> disabled{false};
};

GpuProfiler::GpuProfiler() = default;

GpuProfiler::~GpuProfiler() {
    stop();
}

void GpuProfiler::start(ID3D12Device* device, ID3D12CommandQueue* queue) {
    stop();
    if (g_contexts.fetch_add(1, std::memory_order_relaxed) >= kMaxContexts) {
        std::fprintf(stderr,
                     "D3D12: no GPU zones: this process has created %u Direct3D 12 devices, the most the "
                     "profiler numbers\n",
                     kMaxContexts);
        return;
    }
    const D3D12_QUERY_HEAP_DESC heapDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = kQueryCount};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap)))) {
        std::fprintf(stderr, "D3D12: no GPU zones: creating the profiler's timestamp queries failed\n");
        return;
    }
    heap.Reset();
    auto context = std::make_unique<Context>();
    context->tracy.reset(TracyD3D12Context(device, queue));
    // Tracy gives the context a number only once its query heap, readback buffer and clock
    // calibration exist. In Debug builds a failure there asserts inside Tracy first
    // (TracyD3D12Panic), before this check is reached.
    if (context->tracy->GetId() == tracy::InvalidGpuContextId) {
        std::fprintf(stderr, "D3D12: no GPU zones: creating the profiler's timestamp queries failed\n");
        return;
    }
    TracyD3D12ContextName(context->tracy.get(), kContextName.data(), static_cast<std::uint16_t>(kContextName.size()));
    m_context = std::move(context);
}

void GpuProfiler::stop() {
    m_context.reset();
}

bool GpuProfiler::disabled() const {
    return m_context != nullptr && m_context->disabled.load(std::memory_order_acquire);
}

void GpuProfiler::disable(std::string_view reason) {
    if (m_context != nullptr && !m_context->disabled.exchange(true, std::memory_order_acq_rel)) {
        std::fprintf(stderr, "D3D12: GPU zones are off for this device from now on: %.*s\n",
                     static_cast<int>(reason.size()), reason.data());
    }
}

void GpuProfiler::collect() {
    if (m_context != nullptr) {
        // TracyD3D12NewFrame does nothing in Tracy 0.15.0, so collecting is all a frame needs.
        TracyD3D12Collect(m_context->tracy.get());
    }
}

struct GpuZones::Scopes {
    // Empty for a zone opened while the profiler doesn't run. A scope writes its end timestamp and
    // resolves both into the readback buffer when it is reset.
    std::array<std::optional<tracy::D3D12ZoneScope>, kMaxGpuZoneDepth> open;
    std::uint32_t depth = 0;
    // Whether the current recording opened an active zone, whose queries Tracy waits for.
    bool wrote = false;
};

GpuZones::GpuZones() : m_scopes(std::make_unique<Scopes>()) {}

GpuZones::~GpuZones() {
    ITH_ASSERT_MSG(m_scopes->depth == 0, "a command list was destroyed with GPU zones open");
}

void GpuZones::begin(GpuProfiler& profiler, ID3D12GraphicsCommandList* list, std::string_view name) {
    Scopes& scopes = *m_scopes;
    if (scopes.depth >= kMaxGpuZoneDepth) {
        ITH_ASSERT_MSG(false, "GPU zones nest deeper than kMaxGpuZoneDepth"); // CommandList checks it
        return;
    }
    std::optional<tracy::D3D12ZoneScope>& scope = scopes.open[scopes.depth++];
    if (profiler.m_context != nullptr && !profiler.m_context->disabled.load(std::memory_order_acquire)) {
        // The connection is read once and given to the scope, which can only turn inactive from
        // it: `wrote` is never false for an active scope.
#ifdef TRACY_ON_DEMAND
        const bool active = tracy::GetProfiler().IsConnected();
#else
        const bool active = true;
#endif
        scopes.wrote = scopes.wrote || active;
        // Tracy's own scope, with a transient name: Tracy copies the name while the zone is active
        // (a viewer is connected, or the build collects without one), and otherwise only checks.
        // Its source location is this function's. Unlike the Vulkan backend, there is no engine
        // scope: the query heap, readback buffer and query numbering of Tracy's Direct3D 12
        // context are private to its scope class (docs/development/rhi.md, "GPU zones", Limits).
        scope.emplace(profiler.m_context->tracy.get(), TracyLine, TracyFile, std::strlen(TracyFile), TracyFunction,
                      std::strlen(TracyFunction), name.empty() ? "" : name.data(), std::min(name.size(), kMaxNameSize),
                      list, active);
    }
}

void GpuZones::end() {
    Scopes& scopes = *m_scopes;
    if (scopes.depth == 0) {
        ITH_ASSERT_MSG(false, "endGpuZone() without beginGpuZone()"); // CommandList checks it
        return;
    }
    scopes.open[--scopes.depth].reset();
}

void GpuZones::endAll() {
    while (m_scopes->depth > 0) {
        end();
    }
}

void GpuZones::restart() {
    m_scopes->wrote = false;
}

void GpuZones::abandoned(GpuProfiler& profiler) {
    if (m_scopes->wrote) {
        // Tracy collects zones in order and would wait for these until 32,768 later zones are
        // pending: no zone is opened from now on.
        profiler.disable("a command list that recorded GPU zones never ran (discarded, or its "
                         "submission failed or was skipped); their times are lost");
    }
}

} // namespace ith::rhi::d3d12

#endif
