#pragma once

// Internal: the render graph's declarations, compiled plan and execution state (RenderGraph.h).
// Only the module's own sources include it.

#include "ith/foundation/LinearAllocator.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ith::rg::detail {

// Validation (ADR-0028 rule 20) runs where asserts do: Debug and development builds.
#ifdef ITH_ENABLE_ASSERTS
inline constexpr bool kValidate = true;
#else
inline constexpr bool kValidate = false;
#endif

inline constexpr std::uint32_t kNone = 0xFFFF'FFFFu;

// The write accesses, which a barrier out of Undefined names (Barrier.h).
inline constexpr rhi::Access kWriteAccesses =
    rhi::Access::ShaderWrite | rhi::Access::RenderTargetWrite | rhi::Access::DepthStencilWrite | rhi::Access::CopyWrite;

// A growing chain of linear allocators, rewound at reset: pass names, resource names and execute
// callables live here, so a frame's declarations don't allocate once the blocks exist.
class Arena {
public:
    // Never nullptr: a block large enough is added when the current ones are full.
    void* allocate(std::size_t size, std::size_t alignment);
    std::string_view copy(std::string_view text);
    void reset();

private:
    static constexpr std::size_t kBlockSize = std::size_t{64} * 1024;

    std::vector<std::unique_ptr<LinearAllocator>> m_blocks;
    std::size_t m_current = 0;
};

enum class ResourceKind : std::uint8_t {
    Texture,
    Buffer
};

// A resource's layout, the stages and accesses of its use so far.
struct UseState {
    rhi::TextureLayout layout = rhi::TextureLayout::Undefined;
    rhi::PipelineStage stages = rhi::PipelineStage::None;
    rhi::Access access = rhi::Access::None;
};

struct Resource {
    std::string_view name;
    ResourceKind kind = ResourceKind::Texture;
    bool imported = false;
    bool writable = true;
    // Textures.
    rhi::TextureDimension dimension = rhi::TextureDimension::Texture2D;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t layers = 1;
    std::uint32_t mipLevels = 1;
    rhi::Format format = rhi::Format::Unknown;
    // Buffers.
    std::uint64_t size = 0;
    rhi::MemoryType memory = rhi::MemoryType::GpuOnly;
    // Imports: the usage they were created with. Transients: the union of their alive uses
    // (compile).
    rhi::TextureUsage textureUsage = rhi::TextureUsage::None;
    rhi::BufferUsage bufferUsage = rhi::BufferUsage::None;
    // Imports: the caller's objects. Transients: the pooled objects (execute).
    rhi::TextureHandle texture;
    rhi::BufferHandle buffer;
    ImportedState* state = nullptr;
    ImportedState initial;
    std::optional<rhi::TextureLayout> finalLayout;

    // Compile: the first and last alive pass using it (kNone when unused), its pool slot
    // (transients), and its state after its last use.
    std::uint32_t firstPass = kNone;
    std::uint32_t lastPass = kNone;
    std::uint32_t slot = kNone;
    UseState last;
};

// One resource's use in one pass, merged from the pass's declarations.
struct Use {
    std::uint32_t resource = 0;
    // Bits (1 << TextureUse) or (1 << BufferUse).
    std::uint16_t uses = 0;
    rhi::TextureLayout layout = rhi::TextureLayout::Undefined;
    rhi::PipelineStage stages = rhi::PipelineStage::None;
    rhi::Access access = rhi::Access::None;
    // The RHI usage the use needs.
    rhi::TextureUsage textureUsage = rhi::TextureUsage::None;
    rhi::BufferUsage bufferUsage = rhi::BufferUsage::None;
    bool writes = false;
    // Whether the use depends on the earlier contents (reads, loads and partial writes); false
    // for an attachment cleared or discarded.
    bool keepsContents = true;
    // Attachments.
    bool attachment = false;
    rhi::LoadOp load = rhi::LoadOp::Load;
    rhi::StoreOp store = rhi::StoreOp::Store;
    std::array<float, 4> clearColor{};
    float clearDepth = 0.0f;
};

struct Pass {
    std::string_view name;
    PassType type = PassType::Raster;
    PassFlags flags = PassFlags::None;
    void* object = nullptr;
    void (*call)(void* object, PassContext& context) = nullptr;
    void (*destroy)(void* object) = nullptr;
    // Its uses are uses[firstUse, firstUse + useCount); passes declare them one after another.
    std::uint32_t firstUse = 0;
    std::uint32_t useCount = 0;
    // Compile.
    bool alive = true;
    std::uint32_t group = kNone;
    std::uint32_t firstBarrier = 0;
    std::uint32_t barrierCount = 0;
};

// A barrier of the plan, before a pass or after the graph.
struct PlannedBarrier {
    std::uint32_t resource = 0;
    rhi::PipelineStage stagesBefore = rhi::PipelineStage::None;
    rhi::Access accessBefore = rhi::Access::None;
    rhi::PipelineStage stagesAfter = rhi::PipelineStage::None;
    rhi::Access accessAfter = rhi::Access::None;
    rhi::TextureLayout layoutBefore = rhi::TextureLayout::Undefined;
    rhi::TextureLayout layoutAfter = rhi::TextureLayout::Undefined;
    // The first use of a pooled resource in the frame: the stages and accesses before are those
    // of its holder in an earlier frame, which execute fills in from the pool.
    bool fromPool = false;
    bool resolved = false;
};

// A pooled resource of the frame, which transients with equal descriptions and disjoint lifetimes
// take in turn.
struct Slot {
    // The resource that created the slot, whose description and usage it has.
    std::uint32_t first = 0;
    // The resource holding it, and the last alive pass that holder uses.
    std::uint32_t holder = 0;
    std::uint32_t busyUntil = 0;
    // Execute: the pool entry.
    std::uint32_t entry = kNone;
};

// The barriers and clobber copies execute records for one pass.
struct PassRecording {
    std::uint32_t firstTexture = 0;
    std::uint32_t textureCount = 0;
    std::uint32_t firstBuffer = 0;
    std::uint32_t bufferCount = 0;
    // Clobber: barriers to CopyDst, then copies, before the pass's barriers.
    std::uint32_t firstClobberTexture = 0;
    std::uint32_t clobberTextureCount = 0;
    std::uint32_t firstClobberBuffer = 0;
    std::uint32_t clobberBufferCount = 0;
    std::uint32_t firstClobberResource = 0;
    std::uint32_t clobberResourceCount = 0;
};

struct GraphData {
    explicit GraphData(const RenderGraphDesc& desc);

    // Declaration.
    void reset();
    std::uint32_t addResource(const Resource& resource);
    // The resource of a handle, after checking it belongs to this graph; kNone otherwise.
    std::uint32_t resolve(std::uint32_t index, std::uint32_t handleSerial, ResourceKind kind, std::uint32_t pass);
    void declareTexture(std::uint32_t pass, Texture texture, TextureUse use, rhi::PipelineStage stages, bool write,
                        const Use* attachment);
    void declareBuffer(std::uint32_t pass, Buffer buffer, BufferUse use, rhi::PipelineStage stages, bool write);
    // Merges a use into the pass's use of the same resource, or adds it.
    void addUse(std::uint32_t pass, const Use& use);
    // Whether a pass has room for one more attachment of the use.
    [[nodiscard]] bool attachmentFits(const Pass& pass, TextureUse use) const;
    // Records a validation error: asserts with the message and makes execute() fail.
    void fail(const std::string& message);
    [[nodiscard]] std::string where(std::uint32_t pass, std::uint32_t resource) const;

    // Compile (Compile.cpp).
    void compile();
    void cull();
    void validateOrder();
    void planBarriers();
    void takeSlot(std::uint32_t pass, std::uint32_t r, PlannedBarrier& barrier);
    void planFinalBarriers();

    // Execute (Execute.cpp).
    Expected<void, rhi::Error> execute(ResourcePool& pool, JobSystem* jobs);
    Expected<void, rhi::Error> acquire(ResourcePool& pool);
    void recordClobber(const PassRecording& recording, rhi::CommandList& list) const;
    void beginRendering(const Pass& pass, rhi::CommandList& list) const;
    void prepareRecording();
    void recordGroup(std::uint32_t group, rhi::CommandList& list, const rhi::Device& device) const;
    void finish(ResourcePool& pool);

    // PassContext: the pass's use of a resource, or nullptr.
    [[nodiscard]] const Use* findUse(std::uint32_t pass, std::uint32_t resource) const;
    [[nodiscard]] const Use* contextUse(std::uint32_t pass, std::uint32_t index, std::uint32_t serial,
                                        ResourceKind kind, std::uint16_t allowed, const char* what) const;

    // Dumps (Dump.cpp).
    [[nodiscard]] std::string dump() const;
    [[nodiscard]] std::string dumpGraphviz() const;

    std::string name;
    RenderGraphDesc settings;
    std::uint32_t serial = 0;
    Arena arena;
    std::vector<Resource> resources;
    std::vector<Use> uses;
    std::vector<Pass> passes;
    bool compiled = false;
    bool executed = false;
    // True while a pass's setup callable runs.
    bool inSetup = false;
    // False after a validation error.
    bool valid = true;

    // The plan.
    std::vector<std::uint32_t> alivePasses;
    std::uint32_t groupCount = 0;
    std::vector<PlannedBarrier> barriers;
    std::vector<PlannedBarrier> finalBarriers;
    std::vector<Slot> slots;
    // Scratch, kept for its capacity.
    std::vector<std::uint8_t> needed;
    std::vector<std::uint32_t> openBarriers;

    // Execute.
    std::vector<PassRecording> recordings;
    std::vector<rhi::TextureBarrier> textureBarriers;
    std::vector<rhi::BufferBarrier> bufferBarriers;
    std::vector<rhi::TextureBarrier> finalTextureBarriers;
    std::vector<std::uint32_t> clobberResources;
    std::vector<rhi::CommandList*> lists;
    rhi::BufferHandle clobberSource;
};

// Names for dumps and messages.
[[nodiscard]] std::string_view toString(PassType type);
[[nodiscard]] std::string_view toString(rhi::TextureLayout layout);
[[nodiscard]] std::string_view toString(TextureUse use);
[[nodiscard]] std::string_view toString(BufferUse use);
[[nodiscard]] std::string_view toString(rhi::Format format);
[[nodiscard]] std::string stagesToString(rhi::PipelineStage stages);
[[nodiscard]] std::string accessToString(rhi::Access access);

} // namespace ith::rg::detail
