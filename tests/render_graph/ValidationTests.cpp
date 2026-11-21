// The render graph's validation (ADR-0028 rule 20): every error asserts with the graph, pass and
// resource names, and makes execute() fail with rhi.invalid_usage.

#include "ith/render_graph/RenderGraph.h"

#include "AssertCapture.h"
#include "RenderGraphTestSupport.h"

#include <doctest/doctest.h>

#include <ostream> // doctest prints std::string operands through it
#include <string>

using namespace ith;
using namespace ith::rg;
using ith::rg::test::backbufferDesc;
using ith::rg::test::beginFrame;
using ith::rg::test::endFrame;
using ith::rg::test::fakeBuffer;
using ith::rg::test::fakeTexture;
using ith::rg::test::makeDevice;
using ith::rg::test::nothing;
using ith::rg::test::texture;
using rhi::Format;
using rhi::LoadOp;

// Validation runs where asserts do: Debug and development builds.
#ifdef ITH_ENABLE_ASSERTS

namespace {

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Executing a graph with a validation error records nothing and fails with InvalidUsage.
void checkExecuteFails(RenderGraph& graph) {
    rhi::NullGpu gpu;
    rhi::Device device = makeDevice(gpu);
    ResourcePool pool(device);
    beginFrame(device);
    const Expected<void, rhi::Error> executed = graph.execute(pool);
    REQUIRE_FALSE(executed.hasValue());
    CHECK(executed.error().code == rhi::ErrorCode::InvalidUsage);
    CHECK(gpu.commandCount() == 0);
    endFrame(device);
}

} // namespace

TEST_SUITE("render_graph.validation") {
    TEST_CASE("reading a transient that no earlier pass wrote") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        const Texture out =
            graph.importTexture({.texture = fakeTexture(1), .desc = backbufferDesc(), .writable = true});
        const Texture t = graph.createTexture(texture("Never written"));
        graph.addPass(
            "Reader", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(t, TextureUse::Sampled);
                pass.renderTarget(out);
            },
            nothing());
        graph.compile();
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() ==
              "render graph 'Frame': pass 'Reader', texture 'Never written': reads a transient that no earlier pass "
              "wrote");
        checkExecuteFails(graph);
    }

    TEST_CASE("loading a transient attachment that no earlier pass wrote") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        const Texture t = graph.createTexture(texture("Target"));
        graph.addPass(
            "Load", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(t, LoadOp::Load); }, nothing(),
            PassFlags::NeverCull);
        graph.compile();
        CHECK(capture.count() == 1);
        CHECK(contains(capture.lastMessage(), "pass 'Load', texture 'Target': reads a transient"));
    }

    TEST_CASE("writing a read-only import") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        const Texture asset = graph.importTexture({.texture = fakeTexture(1),
                                                   .desc = {.width = 64,
                                                            .height = 64,
                                                            .format = Format::R8G8B8A8Unorm,
                                                            .usage = rhi::TextureUsage::Storage,
                                                            .debugName = "Asset"}});
        graph.addPass(
            "Writer", PassType::Compute, [&](PassBuilder& pass) { pass.write(asset, TextureUse::Storage); }, nothing());
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() ==
              "render graph 'Frame': pass 'Writer', texture 'Asset': writes a read-only import");
        graph.compile();
        checkExecuteFails(graph);
    }

    TEST_CASE("one resource used twice in a pass with conflicting uses") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        const Texture t = graph.createTexture(texture("Feedback"));
        graph.addPass("Write", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(t); }, nothing());
        graph.addPass(
            "Feedback", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(t, TextureUse::Sampled);
                pass.renderTarget(t, LoadOp::Load);
            },
            nothing());
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() == "render graph 'Frame': pass 'Feedback', texture 'Feedback': used twice in the "
                                       "pass with conflicting uses");

        // A buffer's reads combine; a read and a write don't.
        const Buffer b = graph.createBuffer({.size = 64, .debugName = "B"});
        graph.addPass(
            "Buffer", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.write(b, BufferUse::Storage);
                pass.read(b, BufferUse::ShaderRead);
            },
            nothing());
        CHECK(capture.count() == 2);
    }

    TEST_CASE("uses the description or format forbids") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        const Texture srgb = graph.createTexture(texture("Color", Format::R8G8B8A8Srgb));
        const Texture depth = graph.createTexture(texture("Depth", Format::D32Float));
        const Texture color = graph.createTexture(texture("HDR"));
        graph.addPass(
            "Storage on sRGB", PassType::Compute, [&](PassBuilder& pass) { pass.write(srgb, TextureUse::Storage); },
            nothing());
        CHECK(capture.lastMessage() == "render graph 'Frame': pass 'Storage on sRGB', texture 'Color': Storage is not "
                                       "allowed for the format R8G8B8A8Srgb");
        graph.addPass(
            "Render to depth", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(depth); }, nothing());
        CHECK(contains(capture.lastMessage(), "RenderTarget is not allowed for the format D32Float"));
        graph.addPass("Depth on color", PassType::Raster, [&](PassBuilder& pass) { pass.depth(color); }, nothing());
        CHECK(contains(capture.lastMessage(), "DepthWrite is not allowed for the format R16G16B16A16Float"));
        const Texture imported = graph.importTexture({.texture = fakeTexture(1),
                                                      .desc = {.width = 64,
                                                               .height = 64,
                                                               .format = Format::R8G8B8A8Unorm,
                                                               .usage = rhi::TextureUsage::Sampled,
                                                               .debugName = "Sampled only"},
                                                      .writable = true});
        graph.addPass(
            "Copy into it", PassType::Copy, [&](PassBuilder& pass) { pass.write(imported, TextureUse::CopyDst); },
            nothing());
        CHECK(contains(capture.lastMessage(),
                       "texture 'Sampled only': CopyDst needs a usage the imported texture lacks"));
        graph.addPass(
            "Sample in a copy", PassType::Copy, [&](PassBuilder& pass) { pass.read(imported, TextureUse::Sampled); },
            nothing());
        CHECK(contains(capture.lastMessage(), "a copy pass has no shader uses"));
        graph.addPass(
            "Attach in compute", PassType::Compute, [&](PassBuilder& pass) { pass.renderTarget(color); }, nothing());
        CHECK(contains(capture.lastMessage(), "attachments belong to raster passes"));
        graph.addPass(
            "Read as write", PassType::Compute, [&](PassBuilder& pass) { pass.write(color, TextureUse::Sampled); },
            nothing());
        CHECK(contains(capture.lastMessage(), "Sampled is not a write use"));
        graph.addPass(
            "Compute stage in raster", PassType::Raster,
            [&](PassBuilder& pass) { pass.read(imported, TextureUse::Sampled, rhi::PipelineStage::ComputeShader); },
            nothing());
        CHECK(contains(capture.lastMessage(), "the stages narrow only a shader use"));
        CHECK(capture.count() == 8);
    }

    TEST_CASE("handles of another graph, from before a reset, and null handles") {
        ith::test::AssertCapture capture;
        RenderGraph other({.name = "Other"});
        const Texture foreign = other.createTexture(texture("Foreign"));
        RenderGraph graph({.name = "Frame"});
        const Texture stale = graph.createTexture(texture("Stale"));
        graph.reset();
        graph.addPass(
            "Foreign", PassType::Compute, [&](PassBuilder& pass) { pass.write(foreign, TextureUse::Storage); },
            nothing());
        CHECK(capture.lastMessage() == "render graph 'Frame': pass 'Foreign': a resource handle of another graph, or "
                                       "from before a reset");
        graph.addPass(
            "Stale", PassType::Compute, [&](PassBuilder& pass) { pass.write(stale, TextureUse::Storage); }, nothing());
        graph.addPass(
            "Null", PassType::Compute, [&](PassBuilder& pass) { pass.write(Texture{}, TextureUse::Storage); },
            nothing());
        CHECK(capture.lastMessage() == "render graph 'Frame': pass 'Null': a null resource handle");
        CHECK(capture.count() == 3);
    }

    TEST_CASE("an undeclared resource reached through PassContext") {
        ith::test::AssertCapture capture;
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        RenderGraph graph({.name = "Frame"});
        const Texture a = graph.createTexture(texture("A"));
        const Texture b = graph.createTexture(texture("B"));
        const Buffer out = graph.createBuffer({.size = 64, .debugName = "Out"});
        rhi::TextureHandle undeclared = rhi::TextureHandle::fromParts(1, 1);
        std::uint32_t wrongUse = 0;
        graph.addPass(
            "Write A", PassType::Compute, [&](PassBuilder& pass) { pass.write(a, TextureUse::Storage); }, nothing());
        graph.addPass(
            "Read A", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.read(a, TextureUse::Sampled);
                pass.write(b, TextureUse::Storage);
                pass.write(out, BufferUse::Storage);
            },
            [&, a](PassContext& context) {
                CHECK(context.sampledIndex(a) != rhi::kInvalidDescriptorIndex);
                undeclared = context.texture(Texture::fromParts(a.index() + 100, a.serial()));
                wrongUse = context.storageIndex(a);
                CHECK(context.shaderWriteIndex(out) != rhi::kInvalidDescriptorIndex);
                CHECK(context.shaderReadIndex(out) == rhi::kInvalidDescriptorIndex);
            },
            PassFlags::NeverCull);
        graph.compile();
        CHECK(capture.count() == 0);
        beginFrame(device);
        CHECK(graph.execute(pool).hasValue());
        endFrame(device);
        CHECK(undeclared.isNull());
        CHECK(wrongUse == rhi::kInvalidDescriptorIndex);
        CHECK(capture.count() == 3);
        CHECK(capture.lastMessage() == "render graph 'Frame', pass 'Read A': shaderReadIndex() of buffer 'Out', which "
                                       "the pass didn't declare with a matching use");
    }

    TEST_CASE("attachments of different sizes, and a raster pass without attachments") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        const Texture big = graph.createTexture(texture("Big", Format::R16G16B16A16Float, 64));
        const Texture small = graph.createTexture(texture("Small", Format::D32Float, 32));
        graph.addPass(
            "Mismatch", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.renderTarget(big);
                pass.depth(small);
            },
            nothing(), PassFlags::NeverCull);
        graph.addPass("Nothing to draw into", PassType::Raster, [](PassBuilder&) {}, nothing(), PassFlags::NeverCull);
        graph.compile();
        CHECK(capture.count() == 2);
        CHECK(capture.lastMessage() ==
              "render graph 'Frame': pass 'Nothing to draw into': a raster pass needs a render target or depth "
              "attachment");
    }

    TEST_CASE("bad descriptions and imports") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        CHECK(graph.createTexture({.width = 0, .height = 4, .format = Format::R8Unorm, .debugName = "Empty"}).isNull());
        CHECK(graph.createBuffer({.size = 0, .debugName = "Empty"}).isNull());
        CHECK(graph.importTexture({.desc = backbufferDesc()}).isNull());
        const Texture once = graph.importTexture({.texture = fakeTexture(1), .desc = backbufferDesc()});
        CHECK_FALSE(once.isNull());
        CHECK(graph.importTexture({.texture = fakeTexture(1), .desc = backbufferDesc()}).isNull());
        CHECK(contains(capture.lastMessage(), "the texture is already imported as 'Backbuffer'"));
        CHECK(graph.importBuffer({.buffer = fakeBuffer(1), .desc = {.size = 4, .debugName = "No usage"}}).isNull());
        graph.addPass("", PassType::Compute, [](PassBuilder&) {}, nothing());
        CHECK(capture.lastMessage() == "render graph 'Frame': a pass needs a name");
        CHECK(capture.count() == 6);
    }

    TEST_CASE("a final layout on a read-only import") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        CHECK(graph
                  .importTexture(
                      {.texture = fakeTexture(1), .desc = backbufferDesc(), .finalLayout = rhi::TextureLayout::CopySrc})
                  .isNull());
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() ==
              "render graph 'Frame': importTexture('Backbuffer'): a final layout needs a writable import");
    }

    TEST_CASE("addPass inside another pass's setup callable") {
        ith::test::AssertCapture capture;
        RenderGraph graph({.name = "Frame"});
        const Texture t = graph.createTexture(texture("T"));
        graph.addPass(
            "Outer", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.write(t, TextureUse::Storage);
                graph.addPass("Inner", PassType::Compute, [](PassBuilder&) {}, nothing());
            },
            nothing(), PassFlags::NeverCull);
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() ==
              "render graph 'Frame': addPass() inside a pass's setup callable; add passes one after another");
        CHECK(graph.passCount() == 1);
        // Adding passes one after another still works.
        graph.addPass("Next", PassType::Compute, [](PassBuilder&) {}, nothing(), PassFlags::NeverCull);
        CHECK(graph.passCount() == 2);
        CHECK(capture.count() == 1);
    }
}
#endif
