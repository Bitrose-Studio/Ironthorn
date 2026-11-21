#pragma once

// The backend-neutral state behind rhi::Device and rhi::CommandList. Internal to the rhi module:
// only Device.cpp, CommandList.cpp and the rhi tests include it.

#include "ith/foundation/Compiler.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Mutex.h"
#include "ith/rhi/Backend.h"
#include "ith/rhi/Capabilities.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"
#include "ith/rhi/Handle.h"
#include "ith/rhi/Resources.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ith::rhi::detail {

// True in builds with asserts: the recording checks run only there (ADR-0009 rule 12).
#ifdef ITH_ENABLE_ASSERTS
inline constexpr bool kCheckRecording = true;
#else
inline constexpr bool kCheckRecording = false;
#endif

// A fixed-capacity table of slots with generations, allocated once at device creation so that
// it never moves: other threads read slots without the device mutex.
//
// A slot's generation is odd while an object lives in it and even while it is free. allocate()
// and invalidate() each add 1, so a handle's generation is always odd and a destroyed object's
// handle never matches again. A slot that reaches kRetireGeneration is retired, never reused, so
// generations never wrap.
//
// allocate(), invalidate(), reissue() and release() need the device mutex. They can't carry a
// thread-safety annotation: the table has no mutex of its own, because isLive() and data() are
// read without one. The generation's release and acquire orderings make a destroyed handle
// stale for every thread, but they don't publish the slot data by themselves: the creating
// thread writes the data before it hands the handle out, and the data reaches other threads
// through whatever passes them the handle. It stays unchanged until the slot is allocated
// again, which happens only after the handle went stale. Reading through a handle after its
// destroy() is a caller error, which readers detect by checking the generation again after
// reading the data.
template <typename Data>
class SlotTable {
public:
    // Generations stay below this value; a slot that reaches it is retired.
    static constexpr std::uint32_t kRetireGeneration = 0xFFFF'FFF0u;

    struct Slot {
        std::atomic<std::uint32_t> generation{0};
        Data data{};
    };

    // `firstGeneration` (even) lets tests start near kRetireGeneration.
    explicit SlotTable(std::uint32_t capacity, std::uint32_t firstGeneration = 0) : m_slots(capacity) {
        m_free.reserve(capacity);
        for (std::uint32_t index = capacity; index-- > 0;) {
            m_slots[index].generation.store(firstGeneration, std::memory_order_relaxed);
            m_free.push_back(index); // handed out lowest index first
        }
    }

    // A free slot made live, or nullopt when every slot is taken.
    std::optional<std::uint32_t> allocate() {
        if (m_free.empty()) {
            return std::nullopt;
        }
        const std::uint32_t index = m_free.back();
        m_free.pop_back();
        m_slots[index].generation.fetch_add(1, std::memory_order_release);
        return index;
    }

    // Makes every handle to a live slot stale; the slot stays taken until release().
    void invalidate(std::uint32_t index) { m_slots[index].generation.fetch_add(1, std::memory_order_release); }

    // Makes every handle to a live slot stale and the slot live again under a new generation.
    // Returns false, changing nothing, when that would reach kRetireGeneration.
    [[nodiscard]] bool reissue(std::uint32_t index) {
        if (m_slots[index].generation.load(std::memory_order_relaxed) + 2 >= kRetireGeneration) {
            return false;
        }
        m_slots[index].generation.fetch_add(2, std::memory_order_release);
        return true;
    }

    // Gives back an invalidated slot, unless its generation has reached kRetireGeneration. The
    // data is left as it is: only the thread that allocates the slot next writes it.
    void release(std::uint32_t index) {
        if (m_slots[index].generation.load(std::memory_order_relaxed) < kRetireGeneration) {
            m_free.push_back(index);
        }
    }

    [[nodiscard]] bool isLive(std::uint32_t index, std::uint32_t generation) const {
        return generation != 0 && index < m_slots.size() &&
               m_slots[index].generation.load(std::memory_order_acquire) == generation;
    }

    template <typename Tag>
    [[nodiscard]] bool isLive(Handle<Tag> handle) const {
        return isLive(handle.index(), handle.generation());
    }

    template <typename Tag>
    [[nodiscard]] Handle<Tag> handle(std::uint32_t index) const {
        return Handle<Tag>::fromParts(index, m_slots[index].generation.load(std::memory_order_relaxed));
    }

    [[nodiscard]] bool isLiveIndex(std::uint32_t index) const {
        return (m_slots[index].generation.load(std::memory_order_relaxed) & 1u) != 0;
    }

    [[nodiscard]] std::uint32_t capacity() const { return static_cast<std::uint32_t>(m_slots.size()); }

    Data& data(std::uint32_t index) { return m_slots[index].data; }

    const Data& data(std::uint32_t index) const { return m_slots[index].data; }

private:
    std::vector<Slot> m_slots;
    std::vector<std::uint32_t> m_free;
};

// Hands out bindless descriptor indices of one heap, lowest first. Guarded by the device mutex.
class DescriptorAllocator {
public:
    explicit DescriptorAllocator(std::uint32_t capacity) {
        m_free.reserve(capacity);
        for (std::uint32_t index = capacity; index-- > 0;) {
            m_free.push_back(index);
        }
    }

    std::optional<std::uint32_t> allocate() {
        if (m_free.empty()) {
            return std::nullopt;
        }
        const std::uint32_t index = m_free.back();
        m_free.pop_back();
        return index;
    }

    void release(std::uint32_t index) {
        if (index != kInvalidDescriptorIndex) {
            m_free.push_back(index);
        }
    }

private:
    std::vector<std::uint32_t> m_free;
};

// Slot data stays small, because every table is allocated at full capacity up front (Limits).
// Debug names aren't kept: the backend passes them to the API when it creates the object.
struct BufferSlot {
    std::uint64_t size = 0;
    std::byte* mapped = nullptr;
    BufferDescriptors descriptors;
    BufferUsage usage = BufferUsage::None;
    MemoryType memory = MemoryType::GpuOnly;
};

struct TextureSlot {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t layers = 1;
    std::uint32_t mipLevels = 1;
    TextureDescriptors descriptors;
    TextureDimension dimension = TextureDimension::Texture2D;
    Format format = Format::Unknown;
    TextureUsage usage = TextureUsage::None;
    // A swapchain image: owned by its swapchain, never destroyed through destroy(TextureHandle).
    bool swapchainImage = false;
};

struct SamplerSlot {
    std::uint32_t descriptor = kInvalidDescriptorIndex;
};

enum class PipelineKind : std::uint8_t {
    Graphics,
    Compute
};

struct PipelineSlot {
    PipelineKind kind = PipelineKind::Graphics;
};

// Render-thread data.
struct SwapchainSlot {
    // The texture slots reserved for the images; kept live for the swapchain's lifetime and
    // re-issued (new generations) whenever the images are recreated.
    std::array<std::uint32_t, kMaxSwapchainImages> imageSlots{};
    // count is 0 after a failed recreation, until one succeeds.
    SwapchainImages images;
    // The size resizeSwapchain() asked for.
    std::uint32_t requestedWidth = 0;
    std::uint32_t requestedHeight = 0;
    // The swapchain must be recreated before the next acquire.
    bool outOfDate = false;
    std::string name;
};

enum class ObjectKind : std::uint8_t {
    Buffer,
    Texture,
    Sampler,
    Pipeline
};

// An object whose handle is stale, waiting for its release.
struct Retired {
    ObjectKind kind = ObjectKind::Buffer;
    std::uint32_t index = 0;
    // The timeline value the GPU must reach before the object is released.
    std::uint64_t value = 0;
};

class DeviceState {
public:
    explicit DeviceState(std::unique_ptr<DeviceBackend> backend);
    DeviceState(const DeviceState&) = delete;
    DeviceState& operator=(const DeviceState&) = delete;
    DeviceState(DeviceState&&) = delete;
    DeviceState& operator=(DeviceState&&) = delete;
    ~DeviceState();

    // The functions behind Device; see Device.h for their contracts.
    Expected<BufferHandle, Error> createBuffer(const BufferDesc& desc) ITH_EXCLUDES(m_mutex);
    Expected<TextureHandle, Error> createTexture(const TextureDesc& desc) ITH_EXCLUDES(m_mutex);
    Expected<SamplerHandle, Error> createSampler(const SamplerDesc& desc) ITH_EXCLUDES(m_mutex);
    Expected<PipelineHandle, Error> createGraphicsPipeline(const GraphicsPipelineDesc& desc) ITH_EXCLUDES(m_mutex);
    Expected<PipelineHandle, Error> createComputePipeline(const ComputePipelineDesc& desc) ITH_EXCLUDES(m_mutex);
    void destroy(BufferHandle buffer) ITH_EXCLUDES(m_mutex);
    void destroy(TextureHandle texture) ITH_EXCLUDES(m_mutex);
    void destroy(SamplerHandle sampler) ITH_EXCLUDES(m_mutex);
    void destroy(PipelineHandle pipeline) ITH_EXCLUDES(m_mutex);
    Expected<SwapchainHandle, Error> createSwapchain(const SwapchainDesc& desc) ITH_EXCLUDES(m_mutex);
    void resizeSwapchain(SwapchainHandle swapchain, std::uint32_t width, std::uint32_t height);
    void destroy(SwapchainHandle swapchain) ITH_EXCLUDES(m_mutex);
    Expected<std::optional<Frame>, Error> beginFrame(SwapchainHandle swapchain) ITH_EXCLUDES(m_mutex);
    CommandList& openCommandList(std::string_view debugName) ITH_EXCLUDES(m_mutex);
    Expected<void, Error> submit(std::span<CommandList* const> lists) ITH_EXCLUDES(m_mutex);
    void discard(CommandList& list) ITH_EXCLUDES(m_mutex);
    Expected<void, Error> endFrame() ITH_EXCLUDES(m_mutex);
    Expected<void, Error> waitIdle() ITH_EXCLUDES(m_mutex);
    [[nodiscard]] std::size_t pendingDestructionCount() const ITH_EXCLUDES(m_mutex);

    [[nodiscard]] bool isLost() const { return m_lost.load(std::memory_order_acquire); }

    [[nodiscard]] std::uint64_t frameNumber() const { return m_frameNumber.load(std::memory_order_acquire); }

    // Asserts that the caller is the render thread: the thread that created the device.
    void assertRenderThread() const;

    Capabilities capabilities;
    // Read without the mutex by other threads (see SlotTable).
    SlotTable<BufferSlot> buffers;
    SlotTable<TextureSlot> textures;
    SlotTable<SamplerSlot> samplers;
    SlotTable<PipelineSlot> pipelines;
    SlotTable<SwapchainSlot> swapchains;

private:
    // Returns the device-lost error if the device is lost.
    std::optional<Error> lostError() const ITH_EXCLUDES(m_mutex);
    // Remembers a backend error; DeviceLost marks the device lost.
    Error noteError(Error error) ITH_EXCLUDES(m_mutex);
    // A slot for a new object, or the error why there is none.
    template <typename Data>
    Expected<std::uint32_t, Error> allocateSlot(SlotTable<Data>& table, std::string_view kind, std::string_view name)
        ITH_EXCLUDES(m_mutex);
    // Deferred destruction: queue an object whose handle was just invalidated.
    void retire(ObjectKind kind, std::uint32_t index) ITH_REQUIRES(m_mutex);
    // Releases every retired object the GPU is done with; everything when `all` is set (shutdown
    // or a lost device).
    void collectGarbage(bool all) ITH_EXCLUDES(m_mutex);
    // Calls the backend's destroy and frees the slot and its descriptors.
    void release(ObjectKind kind, std::uint32_t index) ITH_EXCLUDES(m_mutex);
    // Moves objects waiting for open lists to the retired queue once no list is open.
    void closeLists(std::size_t count) ITH_REQUIRES(m_mutex);
    // Recreates a swapchain's images at its requested size, the GPU being idle. On failure the
    // old image handles are stale anyway, and the swapchain stays out of date with no images.
    Expected<void, Error> rebuildSwapchain(std::uint32_t index) ITH_EXCLUDES(m_mutex);
    // New generations for a swapchain's image handles, with their sizes and format. Fails when
    // an image slot has used up its generations.
    Expected<void, Error> reissueImages(SwapchainSlot& swapchain) ITH_EXCLUDES(m_mutex);
    // Writes the image slots' data (size, format, usage) from the swapchain's images.
    void writeImageData(const SwapchainSlot& swapchain);
    static Error invalidUsage(const char* message);

    std::unique_ptr<DeviceBackend> m_backend;
    [[maybe_unused]] const std::thread::id m_renderThread = std::this_thread::get_id(); // read by asserts

    mutable Mutex m_mutex;
    DescriptorAllocator m_resourceDescriptors ITH_GUARDED_BY(m_mutex);
    DescriptorAllocator m_samplerDescriptors ITH_GUARDED_BY(m_mutex);
    std::uint64_t m_lastSubmitted ITH_GUARDED_BY(m_mutex) = 0;
    std::size_t m_openLists ITH_GUARDED_BY(m_mutex) = 0;
    bool m_inFrame ITH_GUARDED_BY(m_mutex) = false;
    std::uint32_t m_frameSlot ITH_GUARDED_BY(m_mutex) = 0;
    // Destroyed while command lists were open: released after the lists are closed and the GPU
    // has finished the submission that closed the last of them.
    std::vector<Retired> m_waitingForLists ITH_GUARDED_BY(m_mutex);
    // Ordered by value.
    std::deque<Retired> m_retired ITH_GUARDED_BY(m_mutex);
    // Per frame slot: the pooled lists and how many of them this frame uses.
    std::array<std::vector<std::unique_ptr<CommandList>>, kFramesInFlight> m_lists ITH_GUARDED_BY(m_mutex);
    std::array<std::size_t, kFramesInFlight> m_listsUsed ITH_GUARDED_BY(m_mutex){};
    std::string m_lostMessage ITH_GUARDED_BY(m_mutex);
    std::atomic<bool> m_lost{false};
    // What openCommandList() returns outside a frame: always failed, never pooled or counted.
    std::unique_ptr<CommandList> m_deadList;

    // Render-thread state; recording threads read the frame number for their checks.
    std::atomic<std::uint64_t> m_frameNumber{0};
    std::array<std::uint64_t, kFramesInFlight> m_frameValues{};
    SwapchainHandle m_frameSwapchain;
};

} // namespace ith::rhi::detail
