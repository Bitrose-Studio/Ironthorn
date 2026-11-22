#include "ith/editor_ui/ImGuiRenderer.h"

#include "ith/editor_ui/EditorUiShader.h"
#include "ith/foundation/Assert.h"
#include "ith/rhi/Pipeline.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

// The official Direct3D 12 and Vulkan backends (MIT) were the behavioural reference for the
// texture requests, the vertex offsets and the scissor projection; this one records RHI commands
// in render graph passes instead of API calls.

namespace ith {
namespace {

static_assert(sizeof(ImDrawVert) == 20, "the shader reads 20-byte vertices: float2 position, float2 uv, packed colour");
static_assert(sizeof(ImDrawIdx) == 2, "the index buffer is bound as 16-bit indices");

// The push constants of EditorUi.slang.
struct Constants {
    std::array<float, 2> scale{};
    std::array<float, 2> translate{};
    std::uint32_t vertices = 0;
    std::uint32_t texture = 0;
    std::uint32_t textureSampler = 0;
    std::uint32_t vertexOffset = 0;
};

static_assert(sizeof(Constants) == kEditorUiConstantsSize);

// The smallest upload buffer, so a quiet frame never needs a new one.
constexpr std::uint64_t kMinimumUploadSize = std::uint64_t{256} * 1024;

constexpr std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// Identifies ImGui's request to reset the render state; every draw sets its whole state anyway.
void resetRenderState(const ImDrawList* /*list*/, const ImDrawCmd* /*command*/) {}

Unexpected<Error> rendererError(const char* what, const rhi::Error& error) {
    return makeError("editor_ui.renderer_failed", std::string(what) + ": " + error.toString());
}

// A command's clip rectangle in framebuffer pixels, clamped to the framebuffer; nothing when no
// whole pixel is left.
std::optional<rhi::Rect> clampedScissor(const ImVec4& clip, ImVec2 origin, ImVec2 pixels, ImVec2 framebuffer) {
    const float left = std::clamp((clip.x - origin.x) * pixels.x, 0.0f, framebuffer.x);
    const float top = std::clamp((clip.y - origin.y) * pixels.y, 0.0f, framebuffer.y);
    const float right = std::clamp((clip.z - origin.x) * pixels.x, 0.0f, framebuffer.x);
    const float bottom = std::clamp((clip.w - origin.y) * pixels.y, 0.0f, framebuffer.y);
    const auto x = static_cast<std::uint32_t>(left);
    const auto y = static_cast<std::uint32_t>(top);
    const std::uint32_t width = right > left ? static_cast<std::uint32_t>(right) - x : 0;
    const std::uint32_t height = bottom > top ? static_cast<std::uint32_t>(bottom) - y : 0;
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    return rhi::Rect{
        .x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y), .width = width, .height = height};
}

// The smallest rectangle holding every draw's scissor.
rhi::Rect scissorBounds(std::span<const UiRenderer::Draw> draws) {
    if (draws.empty()) {
        return {};
    }
    std::int64_t left = draws.front().scissor.x;
    std::int64_t top = draws.front().scissor.y;
    std::int64_t right = left;
    std::int64_t bottom = top;
    for (const UiRenderer::Draw& draw : draws) {
        left = std::min<std::int64_t>(left, draw.scissor.x);
        top = std::min<std::int64_t>(top, draw.scissor.y);
        right = std::max<std::int64_t>(right, draw.scissor.x + std::int64_t{draw.scissor.width});
        bottom = std::max<std::int64_t>(bottom, draw.scissor.y + std::int64_t{draw.scissor.height});
    }
    return {.x = static_cast<std::int32_t>(left),
            .y = static_cast<std::int32_t>(top),
            .width = static_cast<std::uint32_t>(right - left),
            .height = static_cast<std::uint32_t>(bottom - top)};
}

} // namespace

// A texture copy still to be placed in the upload buffer.
struct UiRenderer::PendingUpload {
    ImTextureData* data = nullptr;
    std::uint32_t texture = 0;
    ImTextureRect rect{};
};

UiRenderer::UiRenderer(rhi::Device& device, rhi::PipelineHandle pipeline, rhi::SamplerHandle sampler) noexcept
    : m_device(&device), m_pipeline(pipeline), m_sampler(sampler) {}

Expected<std::unique_ptr<UiRenderer>> UiRenderer::create(rhi::Device& device, rhi::Format targetFormat,
                                                         std::span<const std::byte> vertexShader,
                                                         std::span<const std::byte> pixelShader) {
    const std::array shaders{rhi::ShaderDesc{.stage = rhi::ShaderStage::Vertex,
                                             .code = vertexShader,
                                             .entryPoint = kEditorUiVertexEntry,
                                             .constantsSize = kEditorUiConstantsSize},
                             rhi::ShaderDesc{.stage = rhi::ShaderStage::Pixel,
                                             .code = pixelShader,
                                             .entryPoint = kEditorUiPixelEntry,
                                             .constantsSize = kEditorUiConstantsSize}};
    const std::array targets{rhi::ColorTarget{.format = targetFormat, .blend = rhi::BlendMode::Alpha}};
    Expected<rhi::PipelineHandle, rhi::Error> pipeline = device.createGraphicsPipeline(
        {.shaders = shaders, .colorTargets = targets, .cullMode = rhi::CullMode::None, .debugName = "Editor UI"});
    if (!pipeline) {
        return rendererError("the editor UI pipeline could not be created", pipeline.error());
    }
    Expected<rhi::SamplerHandle, rhi::Error> sampler = device.createSampler({.mipFilter = rhi::Filter::Nearest,
                                                                             .addressU = rhi::AddressMode::ClampToEdge,
                                                                             .addressV = rhi::AddressMode::ClampToEdge,
                                                                             .addressW = rhi::AddressMode::ClampToEdge,
                                                                             .debugName = "Editor UI"});
    if (!sampler) {
        device.destroy(*pipeline);
        return rendererError("the editor UI sampler could not be created", sampler.error());
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "ironthorn_rhi";
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    const auto maxSize = static_cast<int>(device.capabilities().limits.maxTextureDimension2D);
    platformIo.Renderer_TextureMaxWidth = maxSize;
    platformIo.Renderer_TextureMaxHeight = maxSize;
    platformIo.DrawCallback_ResetRenderState = resetRenderState;
    return std::unique_ptr<UiRenderer>(new UiRenderer(device, *pipeline, *sampler));
}

UiRenderer::~UiRenderer() {
    if (ImGui::GetCurrentContext() != nullptr) {
        for (ImTextureData* data : ImGui::GetPlatformIO().Textures) {
            if (data->RefCount == 1 && data->TexID != ImTextureID_Invalid) {
                data->SetTexID(ImTextureID_Invalid);
                data->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
        ImGui::GetPlatformIO().DrawCallback_ResetRenderState = nullptr;
    }
    for (const Texture& texture : m_textures) {
        m_device->destroy(texture.handle);
    }
    for (const FrameBuffer& frame : m_frames) {
        m_device->destroy(frame.buffer);
    }
    m_device->destroy(m_sampler);
    m_device->destroy(m_pipeline);
}

bool UiRenderer::reserve(FrameBuffer& frame, std::uint64_t size) {
    if (frame.buffer != rhi::BufferHandle{} && frame.desc.size >= size) {
        return true;
    }
    // Grown by half again, so a growing UI doesn't make a new buffer every frame.
    const std::uint64_t capacity = std::max(kMinimumUploadSize, alignUp(size + size / 2, 4096));
    const rhi::BufferDesc desc{.size = capacity,
                               .usage =
                                   rhi::BufferUsage::ShaderRead | rhi::BufferUsage::Index | rhi::BufferUsage::CopySrc,
                               .memory = rhi::MemoryType::Upload,
                               .debugName = "Editor UI upload"};
    Expected<rhi::BufferHandle, rhi::Error> buffer = m_device->createBuffer(desc);
    if (!buffer) {
        return false;
    }
    // The old buffer may still be read by the GPU; the device releases it later.
    m_device->destroy(frame.buffer);
    frame = {.buffer = *buffer, .desc = desc, .state = {}};
    return true;
}

void UiRenderer::handleTextureRequests() {
    const ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->Textures == nullptr) {
        return;
    }
    for (ImTextureData* data : *drawData->Textures) {
        if (data->Status == ImTextureStatus_WantCreate) {
            ITH_ASSERT_MSG(data->Format == ImTextureFormat_RGBA32, "the renderer asks ImGui for RGBA textures");
            const rhi::TextureDesc desc{.width = static_cast<std::uint32_t>(data->Width),
                                        .height = static_cast<std::uint32_t>(data->Height),
                                        .format = rhi::Format::R8G8B8A8Unorm,
                                        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
                                        .debugName = "Editor UI texture"};
            Expected<rhi::TextureHandle, rhi::Error> handle = m_device->createTexture(desc);
            if (!handle) {
                continue; // asked again next frame; its draws are skipped
            }
            const auto empty = std::ranges::find_if(
                m_textures, [](const Texture& texture) { return texture.handle == rhi::TextureHandle{}; });
            const auto index = static_cast<std::uint32_t>(empty - m_textures.begin());
            if (empty == m_textures.end()) {
                m_textures.push_back({});
            }
            m_textures[index] = {.handle = *handle, .desc = desc, .state = {}};
            data->SetTexID(static_cast<ImTextureID>(index) + 1);
            m_pending.push_back(
                {.data = data,
                 .texture = index,
                 .rect = {0, 0, static_cast<unsigned short>(data->Width), static_cast<unsigned short>(data->Height)}});
        } else if (data->Status == ImTextureStatus_WantUpdates) {
            const auto index = static_cast<std::uint32_t>(data->TexID - 1);
            for (const ImTextureRect& rect : data->Updates) {
                m_pending.push_back({.data = data, .texture = index, .rect = rect});
            }
        } else if (data->Status == ImTextureStatus_WantDestroy &&
                   std::cmp_greater_equal(data->UnusedFrames, rhi::kFramesInFlight)) {
            const auto index = static_cast<std::size_t>(data->TexID - 1);
            if (data->TexID != ImTextureID_Invalid && index < m_textures.size()) {
                m_device->destroy(m_textures[index].handle);
                m_textures[index] = {};
            }
            data->SetTexID(ImTextureID_Invalid);
            data->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

void UiRenderer::copyGeometry(std::span<std::byte> memory) {
    const ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    const ImVec2 origin = drawData->DisplayPos;
    const ImVec2 extent = drawData->DisplaySize;
    const ImVec2 pixels = drawData->FramebufferScale;
    m_scale = {2.0f / extent.x, -2.0f / extent.y};
    m_translate = {-1.0f - origin.x * m_scale[0], 1.0f - origin.y * m_scale[1]};
    const ImVec2 framebuffer{extent.x * pixels.x, extent.y * pixels.y};
    std::uint64_t vertexBytes = 0;
    std::uint64_t indexBytes = 0;
    std::uint32_t firstVertex = 0;
    std::uint32_t firstIndex = 0;
    for (const ImDrawList* list : drawData->CmdLists) {
        const auto listVertexBytes = static_cast<std::size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        const auto listIndexBytes = static_cast<std::size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        std::memcpy(memory.data() + vertexBytes, list->VtxBuffer.Data, listVertexBytes);
        std::memcpy(memory.data() + m_indexOffset + indexBytes, list->IdxBuffer.Data, listIndexBytes);
        vertexBytes += listVertexBytes;
        indexBytes += listIndexBytes;
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback != nullptr) {
                ITH_ASSERT_MSG(command.UserCallback == resetRenderState,
                               "the editor UI renderer supports only the reset-render-state callback");
                continue;
            }
            const ImTextureID id = command.GetTexID();
            const auto texture = static_cast<std::size_t>(id - 1);
            const std::optional<rhi::Rect> scissor = clampedScissor(command.ClipRect, origin, pixels, framebuffer);
            if (!scissor || command.ElemCount == 0 || id == ImTextureID_Invalid || texture >= m_textures.size() ||
                m_textures[texture].handle == rhi::TextureHandle{}) {
                continue;
            }
            m_draws.push_back({.scissor = *scissor,
                               .texture = static_cast<std::uint32_t>(texture),
                               .indexCount = command.ElemCount,
                               .firstIndex = firstIndex + command.IdxOffset,
                               .vertexOffset = firstVertex + command.VtxOffset});
        }
        firstVertex += static_cast<std::uint32_t>(list->VtxBuffer.Size);
        firstIndex += static_cast<std::uint32_t>(list->IdxBuffer.Size);
    }
}

void UiRenderer::prepare(std::uint32_t frameSlot) {
    ITH_ASSERT_MSG(frameSlot < rhi::kFramesInFlight, "a frame slot is below kFramesInFlight");
    m_slot = frameSlot % rhi::kFramesInFlight;
    m_draws.clear();
    m_uploads.clear();
    m_pending.clear();
    m_prepared = true;
    const ImDrawData* drawData = ImGui::GetDrawData();
    ITH_ASSERT_MSG(drawData != nullptr, "prepare() reads the draw data of ImGui::Render()");

    // Texture requests: creations and destructions now, texel data below.
    handleTextureRequests();

    // The upload buffer's layout: vertices, indices, then each copy's rows at the RHI's alignments.
    const auto vertexCount = static_cast<std::uint64_t>(drawData != nullptr ? drawData->TotalVtxCount : 0);
    const auto indexCount = static_cast<std::uint64_t>(drawData != nullptr ? drawData->TotalIdxCount : 0);
    m_indexOffset = alignUp(vertexCount * sizeof(ImDrawVert), sizeof(std::uint32_t));
    std::uint64_t size = alignUp(m_indexOffset + indexCount * sizeof(ImDrawIdx), rhi::kTextureCopyOffsetAlignment);
    for (const PendingUpload& upload : m_pending) {
        const auto rowPitch =
            static_cast<std::uint32_t>(alignUp(std::uint64_t{upload.rect.w} * 4, rhi::kTextureCopyRowAlignment));
        m_uploads.push_back(
            {.texture = upload.texture,
             .bufferOffset = size,
             .rowPitch = rowPitch,
             .region = {.x = upload.rect.x, .y = upload.rect.y, .width = upload.rect.w, .height = upload.rect.h}});
        size = alignUp(size + std::uint64_t{rowPitch} * upload.rect.h, rhi::kTextureCopyOffsetAlignment);
    }
    FrameBuffer& frame = m_frames[m_slot];
    if (!reserve(frame, size)) {
        // Out of memory: nothing is drawn, and the texture requests are asked again next frame.
        m_uploads.clear();
        m_stats = {.textures = m_stats.textures, .uploadBufferSize = frame.desc.size};
        return;
    }
    const std::span<std::byte> memory = m_device->mappedMemory(frame.buffer);
    for (std::size_t i = 0; i < m_pending.size(); ++i) {
        const PendingUpload& upload = m_pending[i];
        const Upload& placed = m_uploads[i];
        for (std::uint32_t row = 0; row < placed.region.height; ++row) {
            std::memcpy(memory.data() + placed.bufferOffset + std::uint64_t{row} * placed.rowPitch,
                        upload.data->GetPixelsAt(upload.rect.x, upload.rect.y + static_cast<int>(row)),
                        std::size_t{placed.region.width} * 4);
        }
        upload.data->SetStatus(ImTextureStatus_OK);
    }

    // Geometry, and one draw per command with its scissor in framebuffer pixels, clamped to it.
    copyGeometry(memory);

    const auto liveTextures = std::ranges::count_if(
        m_textures, [](const Texture& texture) { return texture.handle != rhi::TextureHandle{}; });
    m_stats = {.draws = static_cast<std::uint32_t>(m_draws.size()),
               .vertices = static_cast<std::uint32_t>(vertexCount),
               .indices = static_cast<std::uint32_t>(indexCount),
               .textureUploads = static_cast<std::uint32_t>(m_uploads.size()),
               .textures = static_cast<std::uint32_t>(liveTextures),
               .uploadBufferSize = frame.desc.size,
               .scissorBounds = scissorBounds(m_draws)};
}

void UiRenderer::addPasses(rg::RenderGraph& graph, rg::Texture target, rhi::LoadOp load,
                           const std::array<float, 4>& clearColor) {
    ITH_ASSERT_MSG(m_prepared, "addPasses() follows prepare()");
    m_prepared = false;
    FrameBuffer& frame = m_frames[m_slot];
    m_graphTextures.assign(m_textures.size(), rg::Texture{});
    for (std::size_t i = 0; i < m_textures.size(); ++i) {
        Texture& texture = m_textures[i];
        if (texture.handle != rhi::TextureHandle{}) {
            m_graphTextures[i] = graph.importTexture(
                {.texture = texture.handle, .desc = texture.desc, .state = &texture.state, .writable = true});
        }
    }
    const rg::Buffer upload =
        frame.buffer != rhi::BufferHandle{}
            ? graph.importBuffer({.buffer = frame.buffer, .desc = frame.desc, .state = &frame.state})
            : rg::Buffer{};

    if (!m_uploads.empty() && !upload.isNull()) {
        graph.addPass(
            "EditorUiUpload", rg::PassType::Copy,
            [&](rg::PassBuilder& pass) {
                pass.read(upload, rg::BufferUse::CopySrc);
                for (const Upload& copy : m_uploads) {
                    pass.write(m_graphTextures[copy.texture], rg::TextureUse::CopyDst);
                }
            },
            [this, upload](rg::PassContext& context) {
                for (const Upload& copy : m_uploads) {
                    context.list().copyBufferToTexture({.buffer = context.buffer(upload),
                                                        .bufferOffset = copy.bufferOffset,
                                                        .rowPitch = copy.rowPitch,
                                                        .texture = context.texture(m_graphTextures[copy.texture]),
                                                        .region = copy.region});
                }
            });
    }

    const std::uint32_t sampler = m_device->samplerIndex(m_sampler);
    graph.addPass(
        "EditorUi", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.renderTarget(target, load, clearColor);
            if (!upload.isNull()) {
                pass.read(upload, rg::BufferUse::ShaderRead, rhi::PipelineStage::VertexShader);
                pass.read(upload, rg::BufferUse::Index);
            }
            for (const rg::Texture texture : m_graphTextures) {
                if (!texture.isNull()) {
                    pass.read(texture, rg::TextureUse::Sampled, rhi::PipelineStage::PixelShader);
                }
            }
        },
        [this, upload, sampler](rg::PassContext& context) {
            if (m_draws.empty() || upload.isNull()) {
                return;
            }
            rhi::CommandList& list = context.list();
            list.setPipeline(m_pipeline);
            list.setIndexBuffer(context.buffer(upload), m_indexOffset, rhi::IndexFormat::Uint16);
            const std::uint32_t vertices = context.shaderReadIndex(upload);
            for (const Draw& draw : m_draws) {
                list.setScissor(draw.scissor);
                list.pushConstants(Constants{.scale = m_scale,
                                             .translate = m_translate,
                                             .vertices = vertices,
                                             .texture = context.sampledIndex(m_graphTextures[draw.texture]),
                                             .textureSampler = sampler,
                                             .vertexOffset = draw.vertexOffset});
                list.drawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
            }
        });
}

} // namespace ith
