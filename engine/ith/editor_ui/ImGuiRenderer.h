#pragma once

// The renderer backend of the editor UI (ADR-0029 rules 9 to 11): Dear ImGui's draw data as two
// render graph passes on the RHI. Part of EditorUi, not of the module's API; no ImGui type appears
// here.
//
// On the main thread, before the graph runs, prepare() handles ImGui's texture requests (create,
// update the changed rectangles, destroy), copies vertices, indices and texel data into the
// upload buffer of the frame in flight (grown when too small), and turns the draw commands into
// UiDraw records with clamped scissors. addPasses() then adds "EditorUiUpload" (Copy: the texture
// copies) and "EditorUi" (Raster: the draws), whose execute callables read only those records.
//
// ImGui's texture ids are indices into the renderer's texture table, plus one (0 is ImGui's
// invalid id). The vertex offset of each draw goes to the shader in the push constants.

#include "ith/editor_ui/EditorUi.h"
#include "ith/foundation/Expected.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ith {

class UiRenderer {
public:
    // Creates the pipeline and the sampler. Fails with "editor_ui.renderer_failed".
    static Expected<std::unique_ptr<UiRenderer>> create(rhi::Device& device, rhi::Format targetFormat,
                                                        std::span<const std::byte> vertexShader,
                                                        std::span<const std::byte> pixelShader);

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;
    UiRenderer(UiRenderer&&) = delete;
    UiRenderer& operator=(UiRenderer&&) = delete;
    // Destroys the textures ImGui still holds, the buffers, the sampler and the pipeline (deferred
    // by the device). Precondition: the ImGui context still exists.
    ~UiRenderer();

    // Reads ImGui's draw data of the last ImGui::Render(); see the header comment.
    void prepare(std::uint32_t frameSlot);

    // Adds the passes for the last prepare().
    void addPasses(rg::RenderGraph& graph, rg::Texture target, rhi::LoadOp load,
                   const std::array<float, 4>& clearColor);

    [[nodiscard]] const UiRenderStats& stats() const noexcept { return m_stats; }

    // One draw: indices from the upload buffer, the shader adding `vertexOffset` to each index.
    struct Draw {
        rhi::Rect scissor;
        // An index into the texture table.
        std::uint32_t texture = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t firstIndex = 0;
        std::uint32_t vertexOffset = 0;
    };

    // One texture copy from the upload buffer.
    struct Upload {
        std::uint32_t texture = 0;
        std::uint64_t bufferOffset = 0;
        std::uint32_t rowPitch = 0;
        rhi::TexelRegion region;
    };

private:
    struct Texture {
        rhi::TextureHandle handle;
        rhi::TextureDesc desc;
        rg::ImportedState state;
    };

    // A frame in flight's upload buffer.
    struct FrameBuffer {
        rhi::BufferHandle buffer;
        rhi::BufferDesc desc;
        rg::ImportedState state;
    };

    // Defined where PendingUpload is complete.
    UiRenderer(rhi::Device& device, rhi::PipelineHandle pipeline, rhi::SamplerHandle sampler) noexcept;

    // A texture copy still to be placed in the upload buffer (ImGuiRenderer.cpp).
    struct PendingUpload;

    // Creates and destroys textures as ImGui asks, and collects the texel data to upload.
    void handleTextureRequests();
    // Copies the vertices and indices into `memory` and turns the draw commands into Draws.
    void copyGeometry(std::span<std::byte> memory);
    // Returns false when the buffer of `slot` can't hold `size` bytes and can't be grown.
    bool reserve(FrameBuffer& frame, std::uint64_t size);

    rhi::Device* m_device;
    rhi::PipelineHandle m_pipeline;
    rhi::SamplerHandle m_sampler;
    // The texture table; a destroyed texture's entry has a null handle and is reused.
    std::vector<Texture> m_textures;
    std::array<FrameBuffer, rhi::kFramesInFlight> m_frames{};
    // The last prepare()'s frame slot, records and projection.
    std::uint32_t m_slot = 0;
    bool m_prepared = false;
    std::vector<Draw> m_draws;
    std::vector<Upload> m_uploads;
    std::vector<PendingUpload> m_pending;
    std::uint64_t m_indexOffset = 0;
    std::array<float, 2> m_scale{};
    std::array<float, 2> m_translate{};
    // This frame's graph handles of the textures, by table index; filled by addPasses().
    std::vector<rg::Texture> m_graphTextures;
    UiRenderStats m_stats;
};

} // namespace ith
