// The text and Graphviz dumps of a compiled render graph (ADR-0028 rule 21).

#include "ith/render_graph/GraphData.h"

#include <array>
#include <string>
#include <utility>

namespace ith::rg::detail {
namespace {

template <typename Enum, std::size_t N>
std::string flagsToString(Enum value, const std::array<std::pair<Enum, std::string_view>, N>& names) {
    std::string text;
    for (const auto& [flag, name] : names) {
        if (rhi::hasAny(value, flag)) {
            if (!text.empty()) {
                text += '|';
            }
            text += name;
        }
    }
    return text.empty() ? std::string("None") : text;
}

std::string textureUsageToString(rhi::TextureUsage usage) {
    using rhi::TextureUsage;
    constexpr std::array<std::pair<TextureUsage, std::string_view>, 6> kNames{{
        {TextureUsage::Sampled, "Sampled"},
        {TextureUsage::Storage, "Storage"},
        {TextureUsage::RenderTarget, "RenderTarget"},
        {TextureUsage::DepthStencil, "DepthStencil"},
        {TextureUsage::CopySrc, "CopySrc"},
        {TextureUsage::CopyDst, "CopyDst"},
    }};
    return flagsToString(usage, kNames);
}

std::string bufferUsageToString(rhi::BufferUsage usage) {
    using rhi::BufferUsage;
    constexpr std::array<std::pair<BufferUsage, std::string_view>, 6> kNames{{
        {BufferUsage::ShaderRead, "ShaderRead"},
        {BufferUsage::ShaderWrite, "ShaderWrite"},
        {BufferUsage::Index, "Index"},
        {BufferUsage::Indirect, "Indirect"},
        {BufferUsage::CopySrc, "CopySrc"},
        {BufferUsage::CopyDst, "CopyDst"},
    }};
    return flagsToString(usage, kNames);
}

// The use bits of a Use, by name.
std::string usesToString(const Use& use, ResourceKind kind) {
    std::string text;
    for (unsigned i = 0; i < 16; ++i) {
        if ((use.uses & (1u << i)) == 0) {
            continue;
        }
        if (!text.empty()) {
            text += '|';
        }
        text +=
            kind == ResourceKind::Texture ? toString(static_cast<TextureUse>(i)) : toString(static_cast<BufferUse>(i));
    }
    return text;
}

std::string_view loadName(rhi::LoadOp load) {
    switch (load) {
        case rhi::LoadOp::Load:
            return "Load";
        case rhi::LoadOp::Clear:
            return "Clear";
        case rhi::LoadOp::DontCare:
            break;
    }
    return "DontCare";
}

std::string_view storeName(rhi::StoreOp store) {
    return store == rhi::StoreOp::Store ? "Store" : "DontCare";
}

std::string passLabel(std::uint32_t index, const Pass& pass) {
    return "pass " + std::to_string(index) + " '" + std::string(pass.name) + "'";
}

// Escapes a name for a quoted Graphviz string.
std::string dotString(std::string_view text) {
    std::string escaped;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

} // namespace

std::string_view toString(PassType type) {
    switch (type) {
        case PassType::Raster:
            return "raster";
        case PassType::Compute:
            return "compute";
        case PassType::Copy:
            break;
    }
    return "copy";
}

std::string_view toString(rhi::TextureLayout layout) {
    switch (layout) {
        case rhi::TextureLayout::Undefined:
            return "Undefined";
        case rhi::TextureLayout::General:
            return "General";
        case rhi::TextureLayout::RenderTarget:
            return "RenderTarget";
        case rhi::TextureLayout::DepthStencilWrite:
            return "DepthStencilWrite";
        case rhi::TextureLayout::DepthStencilRead:
            return "DepthStencilRead";
        case rhi::TextureLayout::ShaderRead:
            return "ShaderRead";
        case rhi::TextureLayout::ShaderWrite:
            return "ShaderWrite";
        case rhi::TextureLayout::CopySrc:
            return "CopySrc";
        case rhi::TextureLayout::CopyDst:
            return "CopyDst";
        case rhi::TextureLayout::Present:
            break;
    }
    return "Present";
}

std::string_view toString(TextureUse use) {
    switch (use) {
        case TextureUse::Sampled:
            return "Sampled";
        case TextureUse::Storage:
            return "Storage";
        case TextureUse::RenderTarget:
            return "RenderTarget";
        case TextureUse::DepthWrite:
            return "DepthWrite";
        case TextureUse::DepthSampled:
            return "DepthSampled";
        case TextureUse::CopySrc:
            return "CopySrc";
        case TextureUse::CopyDst:
            break;
    }
    return "CopyDst";
}

std::string_view toString(BufferUse use) {
    switch (use) {
        case BufferUse::ShaderRead:
            return "ShaderRead";
        case BufferUse::Storage:
            return "Storage";
        case BufferUse::Indirect:
            return "Indirect";
        case BufferUse::Index:
            return "Index";
        case BufferUse::CopySrc:
            return "CopySrc";
        case BufferUse::CopyDst:
            break;
    }
    return "CopyDst";
}

std::string_view toString(rhi::Format format) {
    using rhi::Format;
    switch (format) {
        case Format::Unknown:
            return "Unknown";
        case Format::R8Unorm:
            return "R8Unorm";
        case Format::R8G8Unorm:
            return "R8G8Unorm";
        case Format::R8G8B8A8Unorm:
            return "R8G8B8A8Unorm";
        case Format::R8G8B8A8Srgb:
            return "R8G8B8A8Srgb";
        case Format::B8G8R8A8Unorm:
            return "B8G8R8A8Unorm";
        case Format::B8G8R8A8Srgb:
            return "B8G8R8A8Srgb";
        case Format::R10G10B10A2Unorm:
            return "R10G10B10A2Unorm";
        case Format::R11G11B10Float:
            return "R11G11B10Float";
        case Format::R16G16Float:
            return "R16G16Float";
        case Format::R16G16B16A16Float:
            return "R16G16B16A16Float";
        case Format::R32Uint:
            return "R32Uint";
        case Format::R32Float:
            return "R32Float";
        case Format::R32G32Float:
            return "R32G32Float";
        case Format::R32G32B32A32Float:
            return "R32G32B32A32Float";
        case Format::D32Float:
            return "D32Float";
        case Format::D32FloatS8Uint:
            break;
    }
    return "D32FloatS8Uint";
}

std::string stagesToString(rhi::PipelineStage stages) {
    using rhi::PipelineStage;
    constexpr std::array<std::pair<PipelineStage, std::string_view>, 9> kNames{{
        {PipelineStage::DrawIndirect, "DrawIndirect"},
        {PipelineStage::IndexInput, "IndexInput"},
        {PipelineStage::VertexShader, "VertexShader"},
        {PipelineStage::PixelShader, "PixelShader"},
        {PipelineStage::DepthStencil, "DepthStencil"},
        {PipelineStage::ColorOutput, "ColorOutput"},
        {PipelineStage::ComputeShader, "ComputeShader"},
        {PipelineStage::Copy, "Copy"},
        {PipelineStage::AllCommands, "AllCommands"},
    }};
    return flagsToString(stages, kNames);
}

std::string accessToString(rhi::Access access) {
    using rhi::Access;
    constexpr std::array<std::pair<Access, std::string_view>, 10> kNames{{
        {Access::IndirectArgument, "IndirectArgument"},
        {Access::IndexBuffer, "IndexBuffer"},
        {Access::ShaderRead, "ShaderRead"},
        {Access::ShaderWrite, "ShaderWrite"},
        {Access::RenderTargetRead, "RenderTargetRead"},
        {Access::RenderTargetWrite, "RenderTargetWrite"},
        {Access::DepthStencilRead, "DepthStencilRead"},
        {Access::DepthStencilWrite, "DepthStencilWrite"},
        {Access::CopyRead, "CopyRead"},
        {Access::CopyWrite, "CopyWrite"},
    }};
    return flagsToString(access, kNames);
}

namespace {

std::string resourceLabel(const Resource& resource) {
    std::string text(resource.kind == ResourceKind::Texture ? "texture '" : "buffer '");
    text += resource.name;
    text += '\'';
    return text;
}

std::string_view ownership(const Resource& resource) {
    if (!resource.imported) {
        return "transient";
    }
    return resource.writable ? "imported writable" : "imported read-only";
}

// A texture's size, layers, mips, format and usage, or a buffer's size and usage.
std::string describe(const Resource& resource) {
    if (resource.kind == ResourceKind::Buffer) {
        return " " + std::to_string(resource.size) + " bytes, usage " + bufferUsageToString(resource.bufferUsage);
    }
    std::string text = " " + std::to_string(resource.width) + "x" + std::to_string(resource.height);
    if (resource.dimension == rhi::TextureDimension::Texture3D) {
        text += "x" + std::to_string(resource.depth);
    }
    if (resource.dimension == rhi::TextureDimension::TextureCube) {
        text += " cube";
    }
    if (resource.layers > 1) {
        text += " " + std::to_string(resource.layers) + " layers";
    }
    if (resource.mipLevels > 1) {
        text += " " + std::to_string(resource.mipLevels) + " mips";
    }
    text += ' ';
    text += toString(resource.format);
    text += ", usage " + textureUsageToString(resource.textureUsage);
    return text;
}

std::string resourceLine(std::uint32_t index, const Resource& resource) {
    std::string text = "resource " + std::to_string(index) + " " + resourceLabel(resource) + ": ";
    text += ownership(resource);
    text += describe(resource);
    if (resource.firstPass == kNone) {
        text += ", unused";
    } else {
        text += ", passes " + std::to_string(resource.firstPass) + "-" + std::to_string(resource.lastPass);
    }
    if (resource.slot != kNone) {
        text += ", slot " + std::to_string(resource.slot);
    }
    if (resource.finalLayout) {
        text += ", final ";
        text += toString(*resource.finalLayout);
    }
    text += '\n';
    return text;
}

std::string barrierLine(const PlannedBarrier& barrier, const Resource& resource) {
    std::string text = "  barrier " + resourceLabel(resource) + ": ";
    if (resource.kind == ResourceKind::Texture) {
        text += toString(barrier.layoutBefore);
        text += " -> ";
        text += toString(barrier.layoutAfter);
        text += "; ";
    }
    if (barrier.fromPool && !barrier.resolved) {
        text += "(earlier frame)";
    } else {
        text += "(" + stagesToString(barrier.stagesBefore) + ", " + accessToString(barrier.accessBefore) +
                (barrier.fromPool ? ", earlier frame)" : ")");
    }
    text += " -> (" + stagesToString(barrier.stagesAfter) + ", " + accessToString(barrier.accessAfter) + ")\n";
    return text;
}

std::string useLine(const Use& use, const Resource& resource) {
    std::string text = "  use " + resourceLabel(resource) + ": " + usesToString(use, resource.kind);
    if (use.attachment) {
        text += ", load ";
        text += loadName(use.load);
        text += ", store ";
        text += storeName(use.store);
    }
    text += '\n';
    return text;
}

} // namespace

std::string GraphData::dump() const {
    std::uint32_t culled = 0;
    for (const Pass& pass : passes) {
        culled += pass.alive ? 0 : 1;
    }
    std::string text = "render graph '" + name + "': passes " + std::to_string(passes.size()) + " (" +
                       std::to_string(culled) + " culled), resources " + std::to_string(resources.size()) +
                       ", groups " + std::to_string(groupCount) + "\n";
    for (std::uint32_t p = 0; p < passes.size(); ++p) {
        const Pass& pass = passes[p];
        text += passLabel(p, pass) + " ";
        text += toString(pass.type);
        if (!pass.alive) {
            text += ", culled\n";
            continue;
        }
        text += ", group " + std::to_string(pass.group) + "\n";
        for (std::uint32_t b = pass.firstBarrier; b < pass.firstBarrier + pass.barrierCount; ++b) {
            text += barrierLine(barriers[b], resources[barriers[b].resource]);
        }
        for (std::uint32_t u = pass.firstUse; u < pass.firstUse + pass.useCount; ++u) {
            text += useLine(uses[u], resources[uses[u].resource]);
        }
    }
    if (!finalBarriers.empty()) {
        text += "final\n";
        for (const PlannedBarrier& barrier : finalBarriers) {
            text += barrierLine(barrier, resources[barrier.resource]);
        }
    }
    for (std::uint32_t r = 0; r < resources.size(); ++r) {
        text += resourceLine(r, resources[r]);
    }
    return text;
}

std::string GraphData::dumpGraphviz() const {
    std::string text = "digraph \"" + dotString(name) + "\" {\n  rankdir=LR;\n";
    for (std::uint32_t p = 0; p < passes.size(); ++p) {
        const Pass& pass = passes[p];
        text += "  p" + std::to_string(p) + " [shape=box, " + (pass.alive ? "" : "style=dashed, ") + "label=\"" +
                dotString(pass.name) + (pass.alive ? "" : " (culled)") + "\"];\n";
    }
    for (std::uint32_t r = 0; r < resources.size(); ++r) {
        const Resource& resource = resources[r];
        text += "  r" + std::to_string(r) +
                " [shape=" + (resource.kind == ResourceKind::Texture ? "ellipse" : "hexagon") + ", " +
                (resource.imported ? "style=bold, " : "") + "label=\"" + dotString(resource.name) + "\"];\n";
    }
    for (std::uint32_t p = 0; p < passes.size(); ++p) {
        const Pass& pass = passes[p];
        for (std::uint32_t u = pass.firstUse; u < pass.firstUse + pass.useCount; ++u) {
            const Use& use = uses[u];
            const std::string resource = "r" + std::to_string(use.resource);
            const std::string node = "p" + std::to_string(p);
            text += "  ";
            text += use.writes ? node : resource;
            text += " -> ";
            text += use.writes ? resource : node;
            text += " [label=\"" + usesToString(use, resources[use.resource].kind) + "\"];\n";
        }
    }
    text += "}\n";
    return text;
}

} // namespace ith::rg::detail
