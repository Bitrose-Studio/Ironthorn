#pragma once

// The render graph (ADR-0028): passes declare the resources they use, the graph derives the
// barriers, culls unused passes, pools transient resources and records the passes into command
// lists, in parallel when it is given a job system.
//
//   ith::rg::RenderGraph graph({.name = "Frame"});        // long-lived, reset every frame
//   ith::rg::ResourcePool pool(device);                   // long-lived, one per device
//
//   graph.reset();
//   const rg::Texture backbuffer = graph.importBackbuffer(frame);
//   const rg::Texture sceneColor = graph.createTexture(
//       {.width = w, .height = h, .format = rhi::Format::R16G16B16A16Float, .debugName = "Scene color"});
//   graph.addPass("Scene", rg::PassType::Raster,
//       [&](rg::PassBuilder& pass) { pass.renderTarget(sceneColor, rhi::LoadOp::Clear); },
//       [&scene](rg::PassContext& ctx) { scene.draw(ctx.list()); });
//   graph.addPass("Tonemap", rg::PassType::Raster,
//       [&](rg::PassBuilder& pass) {             // runs now; may capture by reference
//           pass.read(sceneColor, rg::TextureUse::Sampled);
//           pass.renderTarget(backbuffer, rhi::LoadOp::DontCare);
//       },
//       [sceneColor, pipeline](rg::PassContext& ctx) {  // runs at execute; captures by value
//           ctx.list().setPipeline(pipeline);
//           ctx.list().pushConstants(TonemapConstants{.input = ctx.sampledIndex(sceneColor)});
//           ctx.list().draw(3);
//       });
//   graph.compile();
//   if (auto executed = graph.execute(pool, &jobs); !executed) { ... }
//
// A graph is built, compiled and executed once per frame, then reset. Passes run in the order
// they were added; a pass whose writes reach no output is culled. Transient resources come from
// the ResourcePool; imported ones belong to the caller, whose ImportedState carries their state
// from one frame to the next. Validation (Debug and development builds) asserts with the graph,
// pass and resource names.
//
// Threading: a RenderGraph and its ResourcePool belong to the render thread. Execute callables
// run on any thread, in parallel with each other; they must not write shared state.

#include "ith/foundation/Expected.h"
#include "ith/rhi/Barrier.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"
#include "ith/rhi/Format.h"
#include "ith/rhi/Handle.h"
#include "ith/rhi/Resources.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ith {
class JobSystem;
} // namespace ith

namespace ith::rg {

class PassBuilder;
class PassContext;
class RenderGraph;
class ResourcePool;

namespace detail {
struct GraphData;
} // namespace detail

// A handle to a resource of one graph, valid until the graph's reset(). The default handle is
// null. Using a handle of another graph, or one from before a reset, is asserted.
template <typename Tag>
class Handle {
public:
    constexpr Handle() noexcept = default;

    // A handle from its parts; the graph creates handles.
    [[nodiscard]] static constexpr Handle fromParts(std::uint32_t index, std::uint32_t serial) noexcept {
        Handle handle;
        handle.m_index = index;
        handle.m_serial = serial;
        return handle;
    }

    // The resource's index in its graph.
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return m_index; }

    // Which graph, between two resets, created the handle; 0 only for the null handle.
    [[nodiscard]] constexpr std::uint32_t serial() const noexcept { return m_serial; }

    [[nodiscard]] constexpr bool isNull() const noexcept { return m_serial == 0; }

    friend constexpr bool operator==(Handle a, Handle b) noexcept = default;

private:
    std::uint32_t m_index = 0;
    std::uint32_t m_serial = 0;
};

struct TextureTag;
struct BufferTag;

// A texture of the graph: transient (RenderGraph::createTexture) or imported.
using Texture = Handle<TextureTag>;
// A buffer of the graph: transient (RenderGraph::createBuffer) or imported.
using Buffer = Handle<BufferTag>;

enum class PassType : std::uint8_t {
    // Draws inside beginRendering()/endRendering() on its attachments; shader uses default to
    // the vertex and pixel stages.
    Raster,
    // Dispatches; shader uses default to the compute stage.
    Compute,
    // Copies only: no shader uses and no attachments.
    Copy,
};

enum class PassFlags : std::uint8_t {
    None = 0,
    // The pass has an effect the graph can't see (a readback, a timestamp) and is never culled.
    NeverCull = 1u << 0,
};

// How a pass uses a texture, and the layout, stages and accesses that implies (ADR-0028 rule 5).
enum class TextureUse : std::uint8_t {
    // Read in shaders through PassContext::sampledIndex(); ShaderRead layout.
    Sampled,
    // Read and written in shaders through PassContext::storageIndex() (mip 0); ShaderWrite
    // layout. Not for sRGB formats.
    Storage,
    // A color attachment of a raster pass (PassBuilder::renderTarget()).
    RenderTarget,
    // The depth attachment of a raster pass (PassBuilder::depth()).
    DepthWrite,
    // A depth texture read in shaders through PassContext::sampledIndex(); DepthStencilRead
    // layout.
    DepthSampled,
    // The source of a copy; CopySrc layout.
    CopySrc,
    // The destination of a copy; CopyDst layout.
    CopyDst,
};

// How a pass uses a buffer (ADR-0028 rule 5).
enum class BufferUse : std::uint8_t {
    // Read in shaders through PassContext::shaderReadIndex().
    ShaderRead,
    // Read and written in shaders through PassContext::shaderWriteIndex().
    Storage,
    // Indirect draw or dispatch arguments or counts.
    Indirect,
    // An index buffer (CommandList::setIndexBuffer()).
    Index,
    CopySrc,
    CopyDst,
};

// A transient texture: created from the pool for this graph. Its RHI usage is the union of the
// uses passes declare on it.
struct TextureDesc {
    rhi::TextureDimension dimension = rhi::TextureDimension::Texture2D;
    // Positive; depth is 1 except for 3D textures.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    // At least 1; 1 for 3D textures, a multiple of 6 for cubes.
    std::uint32_t layers = 1;
    // At least 1.
    std::uint32_t mipLevels = 1;
    // Not Unknown.
    rhi::Format format = rhi::Format::Unknown;
    // Not empty; copied by the graph.
    std::string_view debugName;
};

// A transient buffer in GPU memory, created from the pool for this graph.
struct BufferDesc {
    // Positive.
    std::uint64_t size = 0;
    // Not empty; copied by the graph.
    std::string_view debugName;
};

// The state of an imported resource between graphs: its layout (textures) and the stages and
// accesses of its last use. The caller keeps one per persistent resource (a history texture, an
// upload buffer); the graph reads it at import and writes the final state back at execute, so
// the next frame's first barrier orders after this frame's last use. A resource that was never
// used starts as the default: Undefined, with nothing to order after. The graph reads it at
// import time, so a resource is used by one graph per frame (all views share that graph); the
// state is not written back when execute() fails.
struct ImportedState {
    rhi::TextureLayout layout = rhi::TextureLayout::Undefined;
    rhi::PipelineStage stages = rhi::PipelineStage::None;
    rhi::Access access = rhi::Access::None;

    friend bool operator==(const ImportedState&, const ImportedState&) = default;
};

// An RHI texture the caller owns, used by the graph.
struct TextureImport {
    // A valid handle; it must stay valid until execute() returns. A texture is imported once
    // per graph.
    rhi::TextureHandle texture;
    // The description the texture was created with (its usage too); the graph checks uses
    // against it and sizes attachments from it.
    rhi::TextureDesc desc;
    // Read at import, written at execute(); must outlive execute(). nullptr: the texture starts
    // in ImportedState's default and its final state is not kept.
    ImportedState* state = nullptr;
    // Whether passes may write it. A pass that writes an imported texture is never culled.
    bool writable = false;
    // The layout the texture must be in after the graph, or std::nullopt to leave it in its
    // last use's layout; needs `writable`, since the transition writes the texture. Present only
    // for swapchain images.
    std::optional<rhi::TextureLayout> finalLayout;
};

// An RHI buffer the caller owns, used by the graph.
struct BufferImport {
    // A valid handle; it must stay valid until execute() returns. A buffer is imported once per
    // graph.
    rhi::BufferHandle buffer;
    // The description the buffer was created with (its usage too).
    rhi::BufferDesc desc;
    // As TextureImport::state; the layout is not used.
    ImportedState* state = nullptr;
    // Whether passes may write it. A pass that writes an imported buffer is never culled.
    bool writable = false;
};

struct RenderGraphDesc {
    // Names the graph in validation messages and dumps; copied.
    std::string_view name = "Render graph";
    // The plan is cut into groups of this many consecutive passes, one command list each,
    // recorded in parallel when execute() has a job system. At least 1.
    std::uint32_t passesPerGroup = 8;
    // Off keeps every pass, for debugging.
    bool culling = true;
    // Debug and development builds: copy a loud value (every byte 0xFF: NaN in float formats,
    // white in UNORM ones) into each transient at its first use, so a pass that reads stale
    // pooled contents shows it. Adds CopyDst to the transients' usage. Ignored in Shipping.
    // Copies cover every mip and layer of the depth or color plane: the stencil plane of
    // D32FloatS8Uint is not clobbered, and block-compressed formats (none in the RHI yet) would
    // need block-sized row pitches the clobber copies don't compute.
    bool clobberTransients = false;
};

// Declares the resources a pass uses; passed to the setup callable of RenderGraph::addPass().
// Every declaration is checked (Debug and development builds): the handle belongs to this
// graph, the resource's description or import allows the use, and the pass type allows it. A
// resource used twice in one pass must be used the same way, except that a buffer may combine
// read uses. `stages` narrows a shader use to some of the pass type's shader stages
// (VertexShader, PixelShader, ComputeShader); None takes them all.
class PassBuilder {
public:
    PassBuilder(const PassBuilder&) = delete;
    PassBuilder& operator=(const PassBuilder&) = delete;
    PassBuilder(PassBuilder&&) = delete;
    PassBuilder& operator=(PassBuilder&&) = delete;
    ~PassBuilder() = default;

    // A read: Sampled, DepthSampled or CopySrc.
    void read(Texture texture, TextureUse use, rhi::PipelineStage stages = rhi::PipelineStage::None);
    // A write: Storage (which also reads) or CopyDst. A whole-texture CopyDst write still keeps
    // the earlier contents, since a copy may cover only part of it.
    void write(Texture texture, TextureUse use, rhi::PipelineStage stages = rhi::PipelineStage::None);
    // A read: ShaderRead, Indirect, Index or CopySrc.
    void read(Buffer buffer, BufferUse use, rhi::PipelineStage stages = rhi::PipelineStage::None);
    // A write: Storage (which also reads) or CopyDst.
    void write(Buffer buffer, BufferUse use, rhi::PipelineStage stages = rhi::PipelineStage::None);

    // A color attachment of a raster pass, in declaration order (at most rhi::kMaxColorTargets).
    // Load keeps the contents (a read); Clear and DontCare discard them. The graph picks the store
    // operation.
    void renderTarget(Texture texture, rhi::LoadOp load = rhi::LoadOp::Clear,
                      const std::array<float, 4>& clearColor = {0.0f, 0.0f, 0.0f, 1.0f});
    // The depth attachment of a raster pass (one at most), written with depth tests. Depth is
    // reversed (ADR-0006): clear to 0.
    void depth(Texture texture, rhi::LoadOp load = rhi::LoadOp::Clear, float clearDepth = 0.0f);

private:
    friend class RenderGraph;

    PassBuilder(detail::GraphData& graph, std::uint32_t pass) : m_graph(&graph), m_pass(pass) {}

    detail::GraphData* m_graph;
    std::uint32_t m_pass;
};

// What an execute callable gets: the command list, and the RHI objects of the resources the
// pass declared. Each accessor asserts (Debug and development builds) that the pass declared the
// resource with a matching use, and returns a null handle or rhi::kInvalidDescriptorIndex when
// it didn't. Its calls are safe on any thread.
class PassContext {
public:
    PassContext(const PassContext&) = delete;
    PassContext& operator=(const PassContext&) = delete;
    PassContext(PassContext&&) = delete;
    PassContext& operator=(PassContext&&) = delete;
    ~PassContext() = default;

    // The list the pass records into. Inside beginRendering() for raster passes, with the
    // viewport and scissor set to the attachments' size; barriers are recorded by the graph.
    [[nodiscard]] rhi::CommandList& list() const { return *m_list; }

    // The device, for the thread-safe queries (descriptor indices, mapped memory).
    [[nodiscard]] const rhi::Device& device() const { return *m_device; }

    [[nodiscard]] std::string_view passName() const;

    // The RHI object of a resource the pass declared, with any use.
    [[nodiscard]] rhi::TextureHandle texture(Texture texture) const;
    [[nodiscard]] rhi::BufferHandle buffer(Buffer buffer) const;
    // Bindless indices; the pass declared the matching use: Sampled or DepthSampled; Storage;
    // ShaderRead; Storage.
    [[nodiscard]] std::uint32_t sampledIndex(Texture texture) const;
    [[nodiscard]] std::uint32_t storageIndex(Texture texture) const;
    [[nodiscard]] std::uint32_t shaderReadIndex(Buffer buffer) const;
    [[nodiscard]] std::uint32_t shaderWriteIndex(Buffer buffer) const;

private:
    friend struct detail::GraphData;

    PassContext(const detail::GraphData& graph, std::uint32_t pass, rhi::CommandList& list, const rhi::Device& device)
        : m_graph(&graph), m_pass(pass), m_list(&list), m_device(&device) {}

    const detail::GraphData* m_graph;
    std::uint32_t m_pass;
    rhi::CommandList* m_list;
    const rhi::Device* m_device;
};

// A frame's passes and resources: declared, compiled into a plan, executed, reset.
//
// Not thread-safe: one thread (the render thread) builds, compiles and executes it. Execute
// callables run on job system workers when execute() has a job system.
class RenderGraph {
public:
    explicit RenderGraph(const RenderGraphDesc& desc = {});
    // Resets the graph.
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;

    // Forgets every pass and resource, destroys the execute callables and makes every handle of
    // the graph stale. Keeps its memory for the next frame.
    void reset();

    // A transient resource, created from the pool at execute. Precondition (asserted; a null
    // handle otherwise): the description's documented rules hold; not compiled yet.
    Texture createTexture(const TextureDesc& desc);
    Buffer createBuffer(const BufferDesc& desc);

    // An imported resource. Precondition (asserted; a null handle otherwise): see TextureImport
    // and BufferImport; not compiled yet.
    Texture importTexture(const TextureImport& import);
    Buffer importBuffer(const BufferImport& import);
    // The frame's swapchain image: writable, from Undefined (acquiring it orders it after its
    // presentation), ending in the Present layout. Precondition: the frame has a backbuffer.
    Texture importBackbuffer(const rhi::Frame& frame);

    // Adds a pass. `setup(PassBuilder&)` runs now and declares the pass's resources; it may
    // capture by reference. `execute(PassContext&)` runs during execute(), maybe on another
    // thread and in parallel with other passes, unless the pass is culled; it should capture by
    // value, is stored in the graph's arena and is destroyed at reset(). Preconditions
    // (asserted): `name` is not empty (it is copied); not compiled yet; not called from inside
    // another pass's setup callable (the pass is then not added).
    template <typename Setup, typename Execute>
    void addPass(std::string_view name, PassType type, Setup&& setup, Execute&& execute,
                 PassFlags flags = PassFlags::None) {
        using Function = std::decay_t<Execute>;
        static_assert(std::is_invocable_v<Function&, PassContext&>, "execute is a callable taking rg::PassContext&");
        static_assert(std::is_invocable_v<Setup&, PassBuilder&>, "setup is a callable taking rg::PassBuilder&");
        if (!canAddPass()) {
            return;
        }
        void* storage = allocateCallable(sizeof(Function), alignof(Function));
        auto* function = ::new (storage) Function(std::forward<Execute>(execute));
        const Callable callable{
            .object = function,
            .call = [](void* object, PassContext& context) { (*static_cast<Function*>(object))(context); },
            .destroy = std::is_trivially_destructible_v<Function>
                           ? nullptr
                           : +[](void* object) { static_cast<Function*>(object)->~Function(); },
        };
        PassBuilder builder(*m_data, beginPass(name, type, flags, callable));
        setupStarted();
        std::invoke(setup, builder);
        setupFinished();
    }

    // Turns the declarations into the plan: order, culling, lifetimes, pool slots, RHI usages,
    // barriers, store operations and command list groups. Needs no device; the same
    // declarations always give the same plan. Validation errors assert, and make execute() fail.
    // Precondition (asserted): not compiled since the last reset().
    void compile();

    // Records the plan into command lists, one per group, in parallel on `jobs` when it has
    // foreground workers and otherwise in order on this thread, and submits them in order with
    // one submit. Takes the transients' resources from `pool` (creating what it lacks) and gives
    // them back, and writes the imports' final states back. Fails with the pool's creation error
    // (rhi.out_of_memory) before recording anything, with rhi.invalid_usage after a validation
    // error, or with the submission's error; after a failure nothing is written back. A frame
    // whose execute() failed before its submission has its backbuffer outside Present: unless
    // the device is lost, execute a graph that only imports the backbuffer before endFrame().
    // Preconditions (asserted): compiled, not executed
    // yet; a frame of the pool's device is open (render thread).
    Expected<void, rhi::Error> execute(ResourcePool& pool, JobSystem* jobs = nullptr);

    // The compiled plan as text: passes in order (culled ones marked) with their group, the
    // barriers before each and their attachments' operations; each resource's kind, lifetime,
    // pool slot and RHI usage; the final transitions. After execute(), barriers from an earlier
    // frame's holder of a pooled resource show that holder's stages. The compile tests compare
    // it. Precondition (asserted; empty otherwise): compiled.
    [[nodiscard]] std::string dump() const;
    // The compiled graph in Graphviz DOT: passes, resources, and read and write edges.
    // Precondition (asserted; empty otherwise): compiled.
    [[nodiscard]] std::string dumpGraphviz() const;

    // The number of passes added, and of command list groups in the compiled plan.
    [[nodiscard]] std::uint32_t passCount() const;
    [[nodiscard]] std::uint32_t groupCount() const;
    // Whether compile() culled pass `pass` (in the order passes were added). Precondition:
    // compiled, pass < passCount().
    [[nodiscard]] bool isCulled(std::uint32_t pass) const;

private:
    struct Callable {
        void* object = nullptr;
        void (*call)(void* object, PassContext& context) = nullptr;
        void (*destroy)(void* object) = nullptr;
    };

    // False, after a validation error, when called from inside a setup callable.
    bool canAddPass();
    void setupStarted();
    void setupFinished();
    void* allocateCallable(std::size_t size, std::size_t alignment);
    std::uint32_t beginPass(std::string_view name, PassType type, PassFlags flags, const Callable& callable);

    std::unique_ptr<detail::GraphData> m_data;
};

} // namespace ith::rg
