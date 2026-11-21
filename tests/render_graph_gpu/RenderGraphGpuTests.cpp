// The render graph on every GPU backend this machine has, with validation on: Direct3D 12's
// debug layer with GPU-based validation, and Vulkan's validation layer with synchronization
// validation, whose errors the backends assert (ADR-0028 rule 23).
//
// What validation can and can't check here: synchronization validation sees attachment accesses,
// copies and layout transitions within one submission. It doesn't see accesses through bindless
// descriptors (sampled and storage reads in shaders) or hazards between submissions, so a wrong
// barrier before a sampled read, or before the first use of a pooled resource after an earlier
// frame, can pass it. Those barriers are checked by the compile and execute tests' dumps
// (tests/render_graph), and the pixels below check the results. The slot-reuse case uses only
// attachments and copies, so its in-frame reuse barrier is one the layer does check.
//
// - Direct3D 12 (Windows): WARP, or the fastest hardware GPU with ITH_RHI_TEST_ADAPTER=hardware;
//   with the debug layer and GPU-based validation outside Shipping (ITH_RHI_TEST_VALIDATION=off
//   turns them off, for machines without the "Graphics Tools").
// - Vulkan: the fastest hardware GPU, or a software one such as lavapipe
//   (ITH_RHI_TEST_ADAPTER=hardware or =software picks one), with the validation layer and
//   synchronization validation where it is installed. Without a Vulkan device its tests report
//   themselves skipped, unless ITH_RHI_TEST_REQUIRE_VULKAN=1.
//
// Each frame of the main case: a depth and HDR color pass, a pass that loads both and draws behind
// the first, an accumulation into a history texture kept across frames, a copy of the result
// through a transient that reuses the HDR target's pooled texture, a tonemap into an imported
// target, and a readback. The pixels are checked against the arithmetic.

#include "ith/jobs/JobSystem.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through it
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace ith::rhi;
using ith::Expected;
using ith::JobSystem;
namespace rg = ith::rg;

namespace {

// The shaders, compiled at build time by ith_embed_shaders.
namespace embedded {
using std::size_t; // the generated headers name it unqualified
#include "combinePixel.spirv.h"
#include "fillPixel.spirv.h"
#include "fullscreenVertex.spirv.h"
#include "tonemapPixel.spirv.h"
#ifdef ITH_RENDER_GRAPH_TEST_HAS_DXIL
#include "combinePixel.dxil.h"
#include "fillPixel.dxil.h"
#include "fullscreenVertex.dxil.h"
#include "tonemapPixel.dxil.h"
#endif
} // namespace embedded

// An environment variable of this suite; empty when unset.
std::string environment(const char* name) {
#ifdef _WIN32
    // getenv_s: the C runtime deprecates getenv on Windows.
    std::array<char, 32> value{};
    std::size_t length = 0;
    if (getenv_s(&length, value.data(), value.size(), name) != 0 || length == 0) {
        return {};
    }
    return value.data();
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): no other thread runs
    return value != nullptr ? value : "";
#endif
}

// A device of `backend`, or std::nullopt when this machine has none (reported, and a failure
// where the backend is required) or creation failed (recorded).
std::optional<Device> makeDevice(Backend backend) {
    const std::string adapter = environment("ITH_RHI_TEST_ADAPTER");
    const bool validation = environment("ITH_RHI_TEST_VALIDATION") != "off";
    if (backend == Backend::D3D12) {
        if (defaultBackend() != Backend::D3D12) {
            MESSAGE("skipped: Direct3D 12 exists only on Windows");
            return std::nullopt;
        }
        Expected<Device, Error> device = createDevice(
            {.adapter = adapter == "hardware" ? AdapterPreference::HighPerformance : AdapterPreference::Software,
             .validation = validation,
             .gpuBasedValidation = validation,
             .applicationName = "render_graph tests"},
            Backend::D3D12);
        if (!device) {
            FAIL(device.error().toString());
            return std::nullopt;
        }
        return std::move(device).value();
    }
    std::vector<AdapterPreference> candidates;
    if (adapter != "software") {
        candidates.push_back(AdapterPreference::HighPerformance);
    }
    if (adapter != "hardware") {
        candidates.push_back(AdapterPreference::Software);
    }
    std::string reasons;
    for (const AdapterPreference candidate : candidates) {
        Expected<Device, Error> device = createDevice(
            {.adapter = candidate, .validation = validation, .applicationName = "render_graph tests"}, Backend::Vulkan);
        if (device) {
            return std::move(device).value();
        }
        reasons += " " + device.error().toString();
    }
    if (environment("ITH_RHI_TEST_REQUIRE_VULKAN") == "1") {
        FAIL("no Vulkan device, and ITH_RHI_TEST_REQUIRE_VULKAN=1 requires one:" << reasons);
    } else {
        MESSAGE("skipped: no Vulkan device:" << reasons);
    }
    return std::nullopt;
}

std::span<const std::byte> bytes(const unsigned char* data, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(data), size};
}

constexpr Format kHdrFormat = Format::R16G16B16A16Float;
constexpr Format kOutputFormat = Format::R8G8B8A8Unorm;
constexpr Format kDepthFormat = Format::D32Float;
// What the scene pass draws, at depth 0.5; the overlay pass draws behind it, at 0.25, and loses.
constexpr std::array<float, 4> kSceneColor{2.0f, 0.5f, 0.25f, 1.0f};
constexpr float kAccumulateWeight = 0.25f;

// The push constants of RenderGraphTest.slang.
struct Constants {
    std::array<float, 4> color{};
    float depth = 0.0f;
    std::uint32_t inputA = 0;
    std::uint32_t inputB = 0;
    float weightA = 0.0f;
    float weightB = 0.0f;
};

static_assert(sizeof(Constants) == 36, "RenderGraphTest.slang's constants are 36 bytes");

struct Pipelines {
    PipelineHandle fill;
    PipelineHandle combine;
    PipelineHandle tonemap;
    // fillPixel into kOutputFormat, without depth.
    PipelineHandle fillOutput;
};

std::optional<Pipelines> createPipelines(Device& device) {
    struct Code {
        std::span<const std::byte> vertex;
        std::span<const std::byte> fill;
        std::span<const std::byte> combine;
        std::span<const std::byte> tonemap;
    };

    Code code{.vertex = bytes(embedded::kFullscreenVertexSpirv, embedded::kFullscreenVertexSpirv_sizeInBytes),
              .fill = bytes(embedded::kFillPixelSpirv, embedded::kFillPixelSpirv_sizeInBytes),
              .combine = bytes(embedded::kCombinePixelSpirv, embedded::kCombinePixelSpirv_sizeInBytes),
              .tonemap = bytes(embedded::kTonemapPixelSpirv, embedded::kTonemapPixelSpirv_sizeInBytes)};
#ifdef ITH_RENDER_GRAPH_TEST_HAS_DXIL
    if (device.capabilities().shaderFormat == ShaderFormat::Dxil) {
        code = {.vertex = bytes(embedded::kFullscreenVertexDxil, embedded::kFullscreenVertexDxil_sizeInBytes),
                .fill = bytes(embedded::kFillPixelDxil, embedded::kFillPixelDxil_sizeInBytes),
                .combine = bytes(embedded::kCombinePixelDxil, embedded::kCombinePixelDxil_sizeInBytes),
                .tonemap = bytes(embedded::kTonemapPixelDxil, embedded::kTonemapPixelDxil_sizeInBytes)};
    }
#endif
    const auto create = [&](std::span<const std::byte> pixel, const char* entryPoint, Format color, bool depth,
                            const char* name) -> std::optional<PipelineHandle> {
        const std::array shaders{ShaderDesc{.stage = ShaderStage::Vertex,
                                            .code = code.vertex,
                                            .entryPoint = "fullscreenVertex",
                                            .constantsSize = sizeof(Constants)},
                                 ShaderDesc{.stage = ShaderStage::Pixel,
                                            .code = pixel,
                                            .entryPoint = entryPoint,
                                            .constantsSize = sizeof(Constants)}};
        const std::array targets{ColorTarget{.format = color}};
        Expected<PipelineHandle, Error> pipeline =
            device.createGraphicsPipeline({.shaders = shaders,
                                           .colorTargets = targets,
                                           .depthFormat = depth ? kDepthFormat : Format::Unknown,
                                           .depth = {.test = depth, .write = depth, .compare = CompareOp::Greater},
                                           .cullMode = CullMode::None,
                                           .debugName = name});
        if (!pipeline) {
            FAIL(pipeline.error().toString());
            return std::nullopt;
        }
        return *pipeline;
    };
    const std::optional<PipelineHandle> fill = create(code.fill, "fillPixel", kHdrFormat, true, "Fill");
    const std::optional<PipelineHandle> combine = create(code.combine, "combinePixel", kHdrFormat, false, "Combine");
    const std::optional<PipelineHandle> tonemap = create(code.tonemap, "tonemapPixel", kOutputFormat, false, "Tonemap");
    const std::optional<PipelineHandle> fillOutput =
        create(code.fill, "fillPixel", kOutputFormat, false, "Fill output");
    if (!fill || !combine || !tonemap || !fillOutput) {
        for (const std::optional<PipelineHandle>& pipeline : {fill, combine, tonemap, fillOutput}) {
            if (pipeline) {
                device.destroy(*pipeline);
            }
        }
        return std::nullopt;
    }
    return Pipelines{.fill = *fill, .combine = *combine, .tonemap = *tonemap, .fillOutput = *fillOutput};
}

// The caller-owned resources the graph imports: the history, the output and the readback
// buffer, each with its state between frames.
struct Imports {
    std::uint32_t size = 0;
    TextureDesc historyDesc;
    TextureDesc outputDesc;
    BufferDesc readbackDesc;
    TextureHandle history;
    TextureHandle output;
    BufferHandle readback;
    rg::ImportedState historyState;
    rg::ImportedState outputState;
    rg::ImportedState readbackState;
    // Frames accumulated into the history since it was created.
    std::uint32_t accumulated = 0;

    [[nodiscard]] std::uint32_t rowPitch() const { return std::max(size * 4, kTextureCopyRowAlignment); }

    void destroy(Device& device) {
        device.destroy(history);
        device.destroy(output);
        device.destroy(readback);
        *this = Imports{};
    }

    // Recreates them at `newSize` when the size changed.
    bool fit(Device& device, std::uint32_t newSize) {
        if (newSize == size) {
            return true;
        }
        destroy(device);
        size = newSize;
        historyDesc = {.width = size,
                       .height = size,
                       .format = kHdrFormat,
                       .usage = TextureUsage::Sampled | TextureUsage::RenderTarget,
                       .debugName = "History"};
        outputDesc = {.width = size,
                      .height = size,
                      .format = kOutputFormat,
                      .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
                      .debugName = "Output"};
        readbackDesc = {.size = std::uint64_t{rowPitch()} * size,
                        .usage = BufferUsage::CopyDst,
                        .memory = MemoryType::Readback,
                        .debugName = "Readback"};
        Expected<TextureHandle, Error> h = device.createTexture(historyDesc);
        Expected<TextureHandle, Error> o = device.createTexture(outputDesc);
        Expected<BufferHandle, Error> r = device.createBuffer(readbackDesc);
        if (!h || !o || !r) {
            FAIL("creating the imported resources failed");
            return false;
        }
        history = *h;
        output = *o;
        readback = *r;
        return true;
    }
};

// Builds and executes one frame. False after recording a failure.
bool renderFrame(Device& device, rg::RenderGraph& graph, rg::ResourcePool& pool, const Pipelines& pipelines,
                 Imports& imports, JobSystem* jobs) {
    Expected<std::optional<Frame>, Error> frame = device.beginFrame();
    if (!frame || !frame->has_value()) {
        FAIL("beginFrame() failed");
        return false;
    }
    graph.reset();
    const std::uint32_t size = imports.size;
    const rg::Texture history = graph.importTexture(
        {.texture = imports.history, .desc = imports.historyDesc, .state = &imports.historyState, .writable = true});
    const rg::Texture output = graph.importTexture(
        {.texture = imports.output, .desc = imports.outputDesc, .state = &imports.outputState, .writable = true});
    const rg::Buffer readback = graph.importBuffer(
        {.buffer = imports.readback, .desc = imports.readbackDesc, .state = &imports.readbackState, .writable = true});
    const auto hdrDesc = [&](const char* name) {
        return rg::TextureDesc{.width = size, .height = size, .format = kHdrFormat, .debugName = name};
    };
    const rg::Texture depth =
        graph.createTexture({.width = size, .height = size, .format = kDepthFormat, .debugName = "Depth"});
    const rg::Texture hdr = graph.createTexture(hdrDesc("HDR"));
    const rg::Texture accum = graph.createTexture(hdrDesc("Accumulated"));
    const rg::Texture post = graph.createTexture(hdrDesc("Post"));
    const auto fill = [&pipelines](float z, const std::array<float, 4>& color) {
        return [fill = pipelines.fill, z, color](rg::PassContext& context) {
            context.list().setPipeline(fill);
            context.list().pushConstants(Constants{.color = color, .depth = z});
            context.list().draw(3);
        };
    };
    // a * weightA + b * weightB.
    const auto combine = [&pipelines](rg::Texture a, float weightA, rg::Texture b, float weightB) {
        return [pipeline = pipelines.combine, a, weightA, b, weightB](rg::PassContext& context) {
            context.list().setPipeline(pipeline);
            context.list().pushConstants(Constants{.inputA = context.sampledIndex(a),
                                                   .inputB = context.sampledIndex(b),
                                                   .weightA = weightA,
                                                   .weightB = weightB});
            context.list().draw(3);
        };
    };

    if (imports.accumulated == 0) {
        graph.addPass(
            "Reset history", rg::PassType::Raster,
            [&](rg::PassBuilder& pass) { pass.renderTarget(history, LoadOp::Clear, {0.0f, 0.0f, 0.0f, 0.0f}); },
            [](rg::PassContext&) {});
    }
    graph.addPass(
        "Scene", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.renderTarget(hdr, LoadOp::Clear);
            pass.depth(depth, LoadOp::Clear);
        },
        fill(0.5f, kSceneColor));
    graph.addPass(
        "Overlay behind", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.renderTarget(hdr, LoadOp::Load);
            pass.depth(depth, LoadOp::Load);
        },
        fill(0.25f, {0.0f, 0.0f, 10.0f, 1.0f}));
    graph.addPass(
        "Accumulate", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.read(history, rg::TextureUse::Sampled, PipelineStage::PixelShader);
            pass.read(hdr, rg::TextureUse::Sampled, PipelineStage::PixelShader);
            pass.renderTarget(accum, LoadOp::DontCare);
        },
        combine(history, 1.0f, hdr, kAccumulateWeight));
    graph.addPass(
        "Update history", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.read(accum, rg::TextureUse::Sampled, PipelineStage::PixelShader);
            pass.renderTarget(history, LoadOp::DontCare);
        },
        combine(accum, 1.0f, accum, 0.0f));
    graph.addPass(
        "Post", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.read(accum, rg::TextureUse::Sampled, PipelineStage::PixelShader);
            pass.renderTarget(post, LoadOp::DontCare);
        },
        combine(accum, 1.0f, accum, 0.0f));
    graph.addPass(
        "Tonemap", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.read(post, rg::TextureUse::Sampled, PipelineStage::PixelShader);
            pass.renderTarget(output, LoadOp::DontCare);
        },
        [pipeline = pipelines.tonemap, post](rg::PassContext& context) {
            context.list().setPipeline(pipeline);
            context.list().pushConstants(Constants{.inputA = context.sampledIndex(post)});
            context.list().draw(3);
        });
    graph.addPass(
        "Readback", rg::PassType::Copy,
        [&](rg::PassBuilder& pass) {
            pass.read(output, rg::TextureUse::CopySrc);
            pass.write(readback, rg::BufferUse::CopyDst);
        },
        [output, readback, rowPitch = imports.rowPitch()](rg::PassContext& context) {
            context.list().copyTextureToBuffer(
                {.buffer = context.buffer(readback), .rowPitch = rowPitch, .texture = context.texture(output)});
        });
    graph.compile();
    const Expected<void, Error> executed = graph.execute(pool, jobs);
    const Expected<void, Error> ended = device.endFrame();
    if (!executed || !ended) {
        FAIL("rendering failed: " << (!executed ? executed.error().toString() : ended.error().toString()));
        return false;
    }
    ++imports.accumulated;
    return true;
}

// Every texel of the readback is the tonemapped accumulation of `frames` frames.
void checkImage(Device& device, const Imports& imports) {
    const std::span<const std::byte> memory = device.mappedMemory(imports.readback);
    REQUIRE(memory.size() >= std::size_t{imports.rowPitch()} * imports.size);
    std::array<int, 4> expected{0, 0, 0, 255};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float c = static_cast<float>(imports.accumulated) * kAccumulateWeight * kSceneColor[channel];
        expected[channel] = static_cast<int>(std::lround(c / (1.0f + c) * 255.0f));
    }
    int wrong = 0;
    std::array<int, 4> first{};
    for (std::uint32_t y = 0; y < imports.size; ++y) {
        for (std::uint32_t x = 0; x < imports.size; ++x) {
            const std::size_t offset = std::size_t{y} * imports.rowPitch() + std::size_t{x} * 4;
            const std::array<int, 4> texel{
                std::to_integer<int>(memory[offset]), std::to_integer<int>(memory[offset + 1]),
                std::to_integer<int>(memory[offset + 2]), std::to_integer<int>(memory[offset + 3])};
            bool close = true;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                close = close && std::abs(texel[channel] - expected[channel]) <= 1;
            }
            if (!close && wrong++ == 0) {
                first = texel;
            }
        }
    }
    CAPTURE(expected);
    CAPTURE(first);
    CHECK(wrong == 0);
}

// Renders `frames` frames at each size in turn, checks the last image of each size, and the pool's
// size after each.
void run(Device& device, const rg::RenderGraphDesc& graphDesc, JobSystem* jobs) {
    const std::optional<Pipelines> pipelines = createPipelines(device);
    REQUIRE(pipelines.has_value());
    if (!pipelines) {
        return;
    }
    rg::ResourcePool pool(device, {.maxUnusedFrames = 4});
    rg::RenderGraph graph(graphDesc);
    Imports imports;
    // Three frames at 64: the history accumulates, and the HDR and post targets share a texture.
    REQUIRE(imports.fit(device, 64));
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(renderFrame(device, graph, pool, *pipelines, imports, jobs));
    }
    CHECK(pool.textureCount() == 3); // depth, HDR and post, accumulated
    REQUIRE(device.waitIdle().hasValue());
    checkImage(device, imports);

    // A resize: new transients; the old ones go after four frames unused.
    REQUIRE(imports.fit(device, 32));
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(renderFrame(device, graph, pool, *pipelines, imports, jobs));
    }
    CHECK(pool.textureCount() == 6);
    REQUIRE(renderFrame(device, graph, pool, *pipelines, imports, jobs));
    CHECK(pool.textureCount() == 3);
    REQUIRE(device.waitIdle().hasValue());
    checkImage(device, imports);

    graph.reset();
    pool.clear();
    imports.destroy(device);
    device.destroy(pipelines->fill);
    device.destroy(pipelines->combine);
    device.destroy(pipelines->tonemap);
    device.destroy(pipelines->fillOutput);
    REQUIRE(device.waitIdle().hasValue());
}

// The texels of an RGBA8 readback are all `expected`, within one level.
void checkUniform(const Device& device, BufferHandle readback, std::uint32_t size, std::uint32_t rowPitch,
                  const std::array<int, 4>& expected) {
    const std::span<const std::byte> memory = device.mappedMemory(readback);
    REQUIRE(memory.size() >= std::size_t{rowPitch} * size);
    int wrong = 0;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::size_t offset = std::size_t{y} * rowPitch + std::size_t{x} * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                wrong += std::abs(std::to_integer<int>(memory[offset + channel]) - expected[channel]) > 1 ? 1 : 0;
            }
        }
    }
    CAPTURE(expected);
    CHECK(wrong == 0);
}

// Slot reuse within a frame that synchronization validation can see: only attachments, copies and
// layout transitions, no bindless accesses. Transient A is cleared as a render target and copied
// into a readback buffer; transient B, with A's description, takes A's pooled texture after that
// copy, is drawn over with DontCare and copied into a second readback buffer. B's first barrier
// must wait for the copy that read A (a write after a read); without it the layer reports a
// hazard. Two frames, so the second frame's A also follows the first frame's B.
void runReuse(Device& device) {
    const std::optional<Pipelines> pipelines = createPipelines(device);
    REQUIRE(pipelines.has_value());
    if (!pipelines) {
        return;
    }
    constexpr std::uint32_t kSize = 64;
    constexpr std::uint32_t kRowPitch = kSize * 4;
    constexpr std::array<float, 4> kClear{1.0f, 0.0f, 0.0f, 1.0f};
    constexpr std::array<float, 4> kDrawn{0.0f, 0.0f, 1.0f, 1.0f};
    const BufferDesc readbackDesc{.size = std::uint64_t{kRowPitch} * kSize,
                                  .usage = BufferUsage::CopyDst,
                                  .memory = MemoryType::Readback,
                                  .debugName = "Readback A"};
    Expected<BufferHandle, Error> readbackA = device.createBuffer(readbackDesc);
    Expected<BufferHandle, Error> readbackB = device.createBuffer({.size = readbackDesc.size,
                                                                   .usage = readbackDesc.usage,
                                                                   .memory = readbackDesc.memory,
                                                                   .debugName = "Readback B"});
    REQUIRE((readbackA && readbackB));
    if (!readbackA || !readbackB) {
        return;
    }
    rg::ImportedState stateA;
    rg::ImportedState stateB;
    rg::ResourcePool pool(device);
    rg::RenderGraph graph({.name = "Reuse"});
    const auto copyInto = [](rg::Texture texture, rg::Buffer buffer) {
        return [texture, buffer](rg::PassContext& context) {
            context.list().copyTextureToBuffer(
                {.buffer = context.buffer(buffer), .rowPitch = kRowPitch, .texture = context.texture(texture)});
        };
    };
    for (int frame = 0; frame < 2; ++frame) {
        Expected<std::optional<Frame>, Error> begun = device.beginFrame();
        REQUIRE((begun && begun->has_value()));
        graph.reset();
        const rg::Buffer outA =
            graph.importBuffer({.buffer = *readbackA, .desc = readbackDesc, .state = &stateA, .writable = true});
        const rg::Buffer outB = graph.importBuffer({.buffer = *readbackB,
                                                    .desc = {.size = readbackDesc.size,
                                                             .usage = readbackDesc.usage,
                                                             .memory = readbackDesc.memory,
                                                             .debugName = "Readback B"},
                                                    .state = &stateB,
                                                    .writable = true});
        const rg::Texture a =
            graph.createTexture({.width = kSize, .height = kSize, .format = kOutputFormat, .debugName = "A"});
        const rg::Texture b =
            graph.createTexture({.width = kSize, .height = kSize, .format = kOutputFormat, .debugName = "B"});
        graph.addPass(
            "Clear A", rg::PassType::Raster,
            [&](rg::PassBuilder& pass) { pass.renderTarget(a, LoadOp::Clear, kClear); }, [](rg::PassContext&) {});
        graph.addPass(
            "Copy A", rg::PassType::Copy,
            [&](rg::PassBuilder& pass) {
                pass.read(a, rg::TextureUse::CopySrc);
                pass.write(outA, rg::BufferUse::CopyDst);
            },
            copyInto(a, outA));
        graph.addPass(
            "Draw B", rg::PassType::Raster, [&](rg::PassBuilder& pass) { pass.renderTarget(b, LoadOp::DontCare); },
            [pipeline = pipelines->fillOutput, kDrawn](rg::PassContext& context) {
                context.list().setPipeline(pipeline);
                context.list().pushConstants(Constants{.color = kDrawn});
                context.list().draw(3);
            });
        graph.addPass(
            "Copy B", rg::PassType::Copy,
            [&](rg::PassBuilder& pass) {
                pass.read(b, rg::TextureUse::CopySrc);
                pass.write(outB, rg::BufferUse::CopyDst);
            },
            copyInto(b, outB));
        graph.compile();
        // B takes A's slot, after the copy that read A.
        const std::string dump = graph.dump();
        CHECK_MESSAGE(dump.find("  barrier texture 'B': Undefined -> RenderTarget; (Copy, None) -> (ColorOutput, "
                                "RenderTargetWrite)\n") != std::string::npos,
                      dump);
        const Expected<void, Error> executed = graph.execute(pool);
        const Expected<void, Error> ended = device.endFrame();
        REQUIRE((executed && ended));
    }
    CHECK(pool.textureCount() == 1);
    REQUIRE(device.waitIdle().hasValue());
    checkUniform(device, *readbackA, kSize, kRowPitch, {255, 0, 0, 255});
    checkUniform(device, *readbackB, kSize, kRowPitch, {0, 0, 255, 255});

    graph.reset();
    pool.clear();
    device.destroy(*readbackA);
    device.destroy(*readbackB);
    device.destroy(pipelines->fill);
    device.destroy(pipelines->combine);
    device.destroy(pipelines->tonemap);
    device.destroy(pipelines->fillOutput);
    REQUIRE(device.waitIdle().hasValue());
}

} // namespace

TEST_SUITE("render_graph_gpu") {
    TEST_CASE("frames through the graph pass validation and give the expected pixels on each backend") {
        for (const Backend backend : {Backend::D3D12, Backend::Vulkan}) {
            CAPTURE(static_cast<int>(backend));
            std::optional<Device> device = makeDevice(backend);
            if (device) {
                run(*device, {.name = "GPU test"}, nullptr);
            }
        }
    }

    TEST_CASE("parallel recording in groups of two, with clobbered transients, on each backend") {
        JobSystem jobs({.foregroundWorkers = 4, .backgroundWorkers = 0});
        for (const Backend backend : {Backend::D3D12, Backend::Vulkan}) {
            CAPTURE(static_cast<int>(backend));
            std::optional<Device> device = makeDevice(backend);
            if (device) {
                run(*device, {.name = "GPU test", .passesPerGroup = 2, .clobberTransients = true}, &jobs);
            }
        }
    }

    TEST_CASE("slot reuse through attachments and copies only, which synchronization validation sees") {
        for (const Backend backend : {Backend::D3D12, Backend::Vulkan}) {
            CAPTURE(static_cast<int>(backend));
            std::optional<Device> device = makeDevice(backend);
            if (device) {
                runReuse(*device);
            }
        }
    }
}
