// Compiling render graphs without a device: order, culling, lifetimes, pool slots, usages,
// barriers, store operations and groups, checked through the text dump (ADR-0028 rule 22).

#include "ith/render_graph/RenderGraph.h"

#include "RenderGraphTestSupport.h"

#include <doctest/doctest.h>

#include <ostream> // doctest prints std::string operands through it
#include <string>

using namespace ith;
using namespace ith::rg;
using ith::rg::test::backbufferDesc;
using ith::rg::test::fakeBuffer;
using ith::rg::test::fakeTexture;
using ith::rg::test::nothing;
using ith::rg::test::texture;
using rhi::Format;
using rhi::LoadOp;
using rhi::PipelineStage;

TEST_SUITE("render_graph.compile") {
    TEST_CASE("a frame: depth prepass, scene, an unused debug view and a tonemap to the backbuffer") {
        RenderGraph graph({.name = "Frame"});
        const Texture backbuffer = graph.importTexture({.texture = fakeTexture(1),
                                                        .desc = backbufferDesc(),
                                                        .writable = true,
                                                        .finalLayout = rhi::TextureLayout::Present});
        const Texture depth = graph.createTexture(texture("Depth", Format::D32Float));
        const Texture scene = graph.createTexture(texture("Scene color"));
        const Texture debug = graph.createTexture(texture("Debug", Format::R8G8B8A8Unorm));
        graph.addPass("Depth", PassType::Raster, [&](PassBuilder& pass) { pass.depth(depth); }, nothing());
        graph.addPass(
            "Scene", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.renderTarget(scene);
                pass.depth(depth, LoadOp::Load);
            },
            nothing());
        graph.addPass(
            "Debug view", PassType::Compute, [&](PassBuilder& pass) { pass.write(debug, TextureUse::Storage); },
            nothing());
        graph.addPass(
            "Tonemap", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(scene, TextureUse::Sampled, PipelineStage::PixelShader);
                pass.renderTarget(backbuffer, LoadOp::DontCare);
            },
            nothing());
        graph.compile();

        CHECK(graph.dump() ==
              "render graph 'Frame': passes 4 (1 culled), resources 4, groups 1\n"
              "pass 0 'Depth' raster, group 0\n"
              "  barrier texture 'Depth': Undefined -> DepthStencilWrite; (earlier frame) -> (DepthStencil, "
              "DepthStencilWrite)\n"
              "  use texture 'Depth': DepthWrite, load Clear, store Store\n"
              "pass 1 'Scene' raster, group 0\n"
              "  barrier texture 'Scene color': Undefined -> RenderTarget; (earlier frame) -> (ColorOutput, "
              "RenderTargetWrite)\n"
              "  barrier texture 'Depth': DepthStencilWrite -> DepthStencilWrite; (DepthStencil, DepthStencilWrite) -> "
              "(DepthStencil, DepthStencilRead|DepthStencilWrite)\n"
              "  use texture 'Scene color': RenderTarget, load Clear, store Store\n"
              "  use texture 'Depth': DepthWrite, load Load, store DontCare\n"
              "pass 2 'Debug view' compute, culled\n"
              "pass 3 'Tonemap' raster, group 0\n"
              "  barrier texture 'Scene color': RenderTarget -> ShaderRead; (ColorOutput, RenderTargetWrite) -> "
              "(PixelShader, ShaderRead)\n"
              "  barrier texture 'Backbuffer': Undefined -> RenderTarget; (None, None) -> (ColorOutput, "
              "RenderTargetWrite)\n"
              "  use texture 'Scene color': Sampled\n"
              "  use texture 'Backbuffer': RenderTarget, load DontCare, store Store\n"
              "final\n"
              "  barrier texture 'Backbuffer': RenderTarget -> Present; (ColorOutput, RenderTargetWrite) -> (None, "
              "None)\n"
              "resource 0 texture 'Backbuffer': imported writable 64x64 B8G8R8A8Srgb, usage RenderTarget|CopyDst, "
              "passes 3-3, final Present\n"
              "resource 1 texture 'Depth': transient 64x64 D32Float, usage DepthStencil, passes 0-1, slot 0\n"
              "resource 2 texture 'Scene color': transient 64x64 R16G16B16A16Float, usage Sampled|RenderTarget, "
              "passes 1-3, slot 1\n"
              "resource 3 texture 'Debug': transient 64x64 R8G8B8A8Unorm, usage None, unused\n");
        CHECK(graph.passCount() == 4);
        CHECK(graph.groupCount() == 1);
        CHECK_FALSE(graph.isCulled(0));
        CHECK(graph.isCulled(2));
    }

    TEST_CASE("the same declarations always give the same plan") {
        const auto build = [](RenderGraph& graph) {
            const Texture out = graph.importTexture({.texture = fakeTexture(1),
                                                     .desc = backbufferDesc(),
                                                     .writable = true,
                                                     .finalLayout = rhi::TextureLayout::Present});
            Texture previous = graph.createTexture(texture("Chain 0"));
            graph.addPass(
                "First", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(previous); }, nothing());
            for (int i = 1; i < 6; ++i) {
                const Texture next = graph.createTexture(texture("Chain"));
                graph.addPass(
                    "Step", PassType::Raster,
                    [&](PassBuilder& pass) {
                        pass.read(previous, TextureUse::Sampled);
                        pass.renderTarget(next);
                    },
                    nothing());
                previous = next;
            }
            graph.addPass(
                "Out", PassType::Raster,
                [&](PassBuilder& pass) {
                    pass.read(previous, TextureUse::Sampled);
                    pass.renderTarget(out);
                },
                nothing());
            graph.compile();
            return graph.dump();
        };
        RenderGraph a({.name = "A"});
        RenderGraph b({.name = "A"});
        const std::string first = build(a);
        CHECK(first == build(b));
        a.reset();
        CHECK(first == build(a));
        // Transients alternate between two slots: each step reads one and writes the other.
        CHECK(first.find("slot 2") == std::string::npos);
        CHECK(first.find("slot 1") != std::string::npos);
    }

    TEST_CASE("culling walks back from outputs and never-cull passes, and can be turned off") {
        const auto build = [](RenderGraph& graph) {
            const Texture a = graph.createTexture(texture("A"));
            const Texture b = graph.createTexture(texture("B"));
            const Buffer readback = graph.importBuffer({.buffer = fakeBuffer(1),
                                                        .desc = {.size = 256,
                                                                 .usage = rhi::BufferUsage::CopyDst,
                                                                 .memory = rhi::MemoryType::Readback,
                                                                 .debugName = "Readback"},
                                                        .writable = true});
            // Writes A, which only a culled pass reads: culled.
            graph.addPass("Write A", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(a); }, nothing());
            // Reads A, writes B, which nothing reads: culled.
            graph.addPass(
                "A to B", PassType::Raster,
                [&](PassBuilder& pass) {
                    pass.read(a, TextureUse::Sampled);
                    pass.renderTarget(b);
                },
                nothing());
            // Writes nothing anyone reads, but has an effect the graph can't see.
            graph.addPass("Timestamp", PassType::Compute, [](PassBuilder&) {}, nothing(), PassFlags::NeverCull);
            // Writes an import: an output.
            graph.addPass(
                "Readback", PassType::Copy, [&](PassBuilder& pass) { pass.write(readback, BufferUse::CopyDst); },
                nothing());
            graph.compile();
        };
        RenderGraph culled;
        build(culled);
        CHECK(culled.isCulled(0));
        CHECK(culled.isCulled(1));
        CHECK_FALSE(culled.isCulled(2));
        CHECK_FALSE(culled.isCulled(3));
        CHECK(culled.groupCount() == 1);
        CHECK(culled.dump().find("resource 0 texture 'A': transient 64x64 R16G16B16A16Float, usage None, unused") !=
              std::string::npos);

        RenderGraph kept({.culling = false});
        build(kept);
        for (std::uint32_t pass = 0; pass < kept.passCount(); ++pass) {
            CHECK_FALSE(kept.isCulled(pass));
        }
        // B is written and never read: its contents aren't stored.
        CHECK(kept.dump().find("use texture 'B': RenderTarget, load Clear, store DontCare") != std::string::npos);
    }

    TEST_CASE("a pass is kept for a later reader; a cleared attachment ends the need for earlier writes") {
        RenderGraph graph({.name = "Overwrite"});
        const Texture out =
            graph.importTexture({.texture = fakeTexture(1), .desc = backbufferDesc(), .writable = true});
        const Texture t = graph.createTexture(texture("T"));
        graph.addPass("Clear T", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(t); }, nothing());
        graph.addPass("Clear T again", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(t); }, nothing());
        graph.addPass(
            "Draw over T", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(t, LoadOp::Load); }, nothing());
        graph.addPass(
            "Use T", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(t, TextureUse::Sampled);
                pass.renderTarget(out, LoadOp::Load);
            },
            nothing());
        graph.compile();
        CHECK(graph.isCulled(0));
        CHECK_FALSE(graph.isCulled(1));
        CHECK_FALSE(graph.isCulled(2));
        CHECK_FALSE(graph.isCulled(3));
        CHECK(graph.dump() ==
              "render graph 'Overwrite': passes 4 (1 culled), resources 2, groups 1\n"
              "pass 0 'Clear T' raster, culled\n"
              "pass 1 'Clear T again' raster, group 0\n"
              "  barrier texture 'T': Undefined -> RenderTarget; (earlier frame) -> (ColorOutput, RenderTargetWrite)\n"
              "  use texture 'T': RenderTarget, load Clear, store Store\n"
              "pass 2 'Draw over T' raster, group 0\n"
              "  barrier texture 'T': RenderTarget -> RenderTarget; (ColorOutput, RenderTargetWrite) -> (ColorOutput, "
              "RenderTargetRead|RenderTargetWrite)\n"
              "  use texture 'T': RenderTarget, load Load, store Store\n"
              "pass 3 'Use T' raster, group 0\n"
              "  barrier texture 'T': RenderTarget -> ShaderRead; (ColorOutput, RenderTargetRead|RenderTargetWrite) -> "
              "(VertexShader|PixelShader, ShaderRead)\n"
              "  barrier texture 'Backbuffer': Undefined -> RenderTarget; (None, None) -> (ColorOutput, "
              "RenderTargetRead|RenderTargetWrite)\n"
              "  use texture 'T': Sampled\n"
              "  use texture 'Backbuffer': RenderTarget, load Load, store Store\n"
              "resource 0 texture 'Backbuffer': imported writable 64x64 B8G8R8A8Srgb, usage RenderTarget|CopyDst, "
              "passes 3-3\n"
              "resource 1 texture 'T': transient 64x64 R16G16B16A16Float, usage Sampled|RenderTarget, passes 1-3, "
              "slot 0\n");
    }

    TEST_CASE("transients with equal descriptions and disjoint lifetimes share a slot, after the earlier one's use") {
        RenderGraph graph({.name = "Reuse"});
        const Texture out = graph.importTexture({.texture = fakeTexture(1),
                                                 .desc = {.width = 64,
                                                          .height = 64,
                                                          .format = Format::R16G16B16A16Float,
                                                          .usage = rhi::TextureUsage::Storage,
                                                          .debugName = "Output"},
                                                 .writable = true});
        const Texture a = graph.createTexture(texture("A"));
        const Texture b = graph.createTexture(texture("B"));
        const Texture c = graph.createTexture(texture("C"));
        const Texture smaller = graph.createTexture(texture("Smaller", Format::R16G16B16A16Float, 32));
        graph.addPass(
            "Fill A", PassType::Compute, [&](PassBuilder& pass) { pass.write(a, TextureUse::Storage); }, nothing());
        graph.addPass(
            "A to B", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.read(a, TextureUse::Sampled);
                pass.write(b, TextureUse::Storage);
            },
            nothing());
        // A is free from here: C takes its slot. B is still alive.
        graph.addPass(
            "B to C", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.read(b, TextureUse::Sampled);
                pass.write(c, TextureUse::Storage);
            },
            nothing());
        graph.addPass(
            "C to smaller", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.read(c, TextureUse::Sampled);
                pass.write(smaller, TextureUse::Storage);
            },
            nothing());
        graph.addPass(
            "Smaller to output", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.read(smaller, TextureUse::Sampled);
                pass.write(out, TextureUse::Storage);
            },
            nothing());
        graph.compile();
        const std::string dump = graph.dump();
        CHECK(dump.find("resource 1 texture 'A': transient 64x64 R16G16B16A16Float, usage Sampled|Storage, passes "
                        "0-1, slot 0\n") != std::string::npos);
        CHECK(dump.find("resource 2 texture 'B': transient 64x64 R16G16B16A16Float, usage Sampled|Storage, passes "
                        "1-2, slot 1\n") != std::string::npos);
        CHECK(dump.find("resource 3 texture 'C': transient 64x64 R16G16B16A16Float, usage Sampled|Storage, passes "
                        "2-3, slot 0\n") != std::string::npos);
        CHECK(dump.find("resource 4 texture 'Smaller': transient 32x32 R16G16B16A16Float, usage Sampled|Storage, "
                        "passes 3-4, slot 2\n") != std::string::npos);
        // C's first barrier discards A's contents after A's last use, a read: stages only.
        CHECK(dump.find("  barrier texture 'C': Undefined -> ShaderWrite; (ComputeShader, None) -> (ComputeShader, "
                        "ShaderRead|ShaderWrite)\n") != std::string::npos);
        // A's slot, and B's and the smaller one's, come from earlier frames.
        CHECK(dump.find("  barrier texture 'A': Undefined -> ShaderWrite; (earlier frame) -> (ComputeShader, "
                        "ShaderRead|ShaderWrite)\n") != std::string::npos);
    }

    TEST_CASE("consecutive reads in one layout merge their stages into one barrier") {
        RenderGraph graph({.name = "Reads"});
        const Texture out =
            graph.importTexture({.texture = fakeTexture(1), .desc = backbufferDesc(), .writable = true});
        const Texture t = graph.createTexture(texture("T"));
        graph.addPass(
            "Write", PassType::Compute, [&](PassBuilder& pass) { pass.write(t, TextureUse::Storage); }, nothing());
        graph.addPass(
            "Read in pixels", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(t, TextureUse::Sampled, PipelineStage::PixelShader);
                pass.renderTarget(out, LoadOp::Load);
            },
            nothing());
        graph.addPass(
            "Read in compute", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.read(t, TextureUse::Sampled);
                pass.write(out, TextureUse::CopyDst);
            },
            nothing());
        graph.addPass(
            "Write again", PassType::Compute,
            [&](PassBuilder& pass) {
                pass.write(t, TextureUse::Storage);
                pass.write(out, TextureUse::CopyDst);
            },
            nothing());
        graph.compile();
        const std::string dump = graph.dump();
        CHECK(dump.find("pass 1 'Read in pixels' raster, group 0\n"
                        "  barrier texture 'T': ShaderWrite -> ShaderRead; (ComputeShader, ShaderRead|ShaderWrite) -> "
                        "(PixelShader|ComputeShader, ShaderRead)\n") != std::string::npos);
        CHECK(dump.find("pass 2 'Read in compute' compute, group 0\n"
                        "  barrier texture 'Backbuffer'") != std::string::npos);
        CHECK(dump.find("pass 3 'Write again' compute, group 0\n"
                        "  barrier texture 'T': ShaderRead -> ShaderWrite; (PixelShader|ComputeShader, ShaderRead) -> "
                        "(ComputeShader, ShaderRead|ShaderWrite)\n") != std::string::npos);
    }

    TEST_CASE("buffers: read uses combine in one pass, and barriers follow writes, not layouts") {
        RenderGraph graph({.name = "Buffers"});
        const Buffer upload = graph.importBuffer({.buffer = fakeBuffer(1),
                                                  .desc = {.size = 1024,
                                                           .usage = rhi::BufferUsage::CopySrc,
                                                           .memory = rhi::MemoryType::Upload,
                                                           .debugName = "Upload"}});
        const Texture out =
            graph.importTexture({.texture = fakeTexture(1), .desc = backbufferDesc(), .writable = true});
        const Buffer args = graph.createBuffer({.size = 1024, .debugName = "Arguments"});
        graph.addPass(
            "Upload", PassType::Copy,
            [&](PassBuilder& pass) {
                pass.read(upload, BufferUse::CopySrc);
                pass.write(args, BufferUse::CopyDst);
            },
            nothing());
        graph.addPass(
            "Draw", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(args, BufferUse::Indirect);
                pass.read(args, BufferUse::ShaderRead, PipelineStage::VertexShader);
                pass.renderTarget(out);
            },
            nothing());
        graph.compile();
        CHECK(graph.dump() ==
              "render graph 'Buffers': passes 2 (0 culled), resources 3, groups 1\n"
              "pass 0 'Upload' copy, group 0\n"
              "  barrier buffer 'Arguments': (earlier frame) -> (Copy, CopyWrite)\n"
              "  use buffer 'Upload': CopySrc\n"
              "  use buffer 'Arguments': CopyDst\n"
              "pass 1 'Draw' raster, group 0\n"
              "  barrier buffer 'Arguments': (Copy, CopyWrite) -> (DrawIndirect|VertexShader, "
              "IndirectArgument|ShaderRead)\n"
              "  barrier texture 'Backbuffer': Undefined -> RenderTarget; (None, None) -> (ColorOutput, "
              "RenderTargetWrite)\n"
              "  use buffer 'Arguments': ShaderRead|Indirect\n"
              "  use texture 'Backbuffer': RenderTarget, load Clear, store Store\n"
              "resource 0 buffer 'Upload': imported read-only 1024 bytes, usage CopySrc, passes 0-0\n"
              "resource 1 texture 'Backbuffer': imported writable 64x64 B8G8R8A8Srgb, usage RenderTarget|CopyDst, "
              "passes 1-1\n"
              "resource 2 buffer 'Arguments': transient 1024 bytes, usage ShaderRead|Indirect|CopyDst, passes 0-1, "
              "slot 0\n");
    }

    TEST_CASE("an import starts from its state and ends in its final layout") {
        RenderGraph graph({.name = "History"});
        ImportedState state{.layout = rhi::TextureLayout::RenderTarget,
                            .stages = PipelineStage::ColorOutput,
                            .access = rhi::Access::RenderTargetWrite};
        const Texture history =
            graph.importTexture({.texture = fakeTexture(1),
                                 .desc = {.width = 64,
                                          .height = 64,
                                          .format = Format::R16G16B16A16Float,
                                          .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::RenderTarget,
                                          .debugName = "History"},
                                 .state = &state,
                                 .writable = true,
                                 .finalLayout = rhi::TextureLayout::ShaderRead});
        // Imported and never used.
        static_cast<void>(graph.importTexture({.texture = fakeTexture(2),
                                               .desc = {.width = 64,
                                                        .height = 64,
                                                        .format = Format::R8G8B8A8Srgb,
                                                        .usage = rhi::TextureUsage::Sampled,
                                                        .debugName = "Asset"},
                                               .state = nullptr,
                                               .writable = true,
                                               .finalLayout = rhi::TextureLayout::ShaderRead}));
        const Texture out =
            graph.importTexture({.texture = fakeTexture(3), .desc = backbufferDesc(), .writable = true});
        graph.addPass(
            "Resolve", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(history, TextureUse::Sampled, PipelineStage::PixelShader);
                pass.renderTarget(out);
            },
            nothing());
        graph.compile();
        const std::string dump = graph.dump();
        // The history was written as a render target last frame.
        CHECK(dump.find("  barrier texture 'History': RenderTarget -> ShaderRead; (ColorOutput, RenderTargetWrite) -> "
                        "(PixelShader, ShaderRead)\n") != std::string::npos);
        // Already in its final layout: no final barrier. The unused asset still gets one.
        CHECK(dump.find("final\n"
                        "  barrier texture 'Asset': Undefined -> ShaderRead; (None, None) -> (AllCommands, "
                        "ShaderRead)\n"
                        "resource 0") != std::string::npos);
        CHECK(dump.find("resource 1 texture 'Asset': imported writable 64x64 R8G8B8A8Srgb, usage Sampled, unused, "
                        "final ShaderRead\n") != std::string::npos);
    }

    TEST_CASE("groups cut the plan every passesPerGroup alive passes") {
        RenderGraph graph({.name = "Groups", .passesPerGroup = 2});
        const Texture out =
            graph.importTexture({.texture = fakeTexture(1), .desc = backbufferDesc(), .writable = true});
        const Texture unused = graph.createTexture(texture("Unused"));
        graph.addPass("Culled", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(unused); }, nothing());
        for (int i = 0; i < 5; ++i) {
            graph.addPass(
                "Draw", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(out, LoadOp::Load); }, nothing());
        }
        graph.compile();
        CHECK(graph.groupCount() == 3);
        const std::string dump = graph.dump();
        CHECK(dump.find("pass 1 'Draw' raster, group 0\n") != std::string::npos);
        CHECK(dump.find("pass 2 'Draw' raster, group 0\n") != std::string::npos);
        CHECK(dump.find("pass 3 'Draw' raster, group 1\n") != std::string::npos);
        CHECK(dump.find("pass 5 'Draw' raster, group 2\n") != std::string::npos);
    }

    TEST_CASE("an empty graph compiles to an empty plan") {
        RenderGraph graph({.name = "Empty"});
        graph.compile();
        CHECK(graph.groupCount() == 0);
        CHECK(graph.dump() == "render graph 'Empty': passes 0 (0 culled), resources 0, groups 0\n");
    }

    TEST_CASE("the Graphviz dump has every pass, resource and use") {
        RenderGraph graph({.name = "Dot \"graph\""});
        const Texture out =
            graph.importTexture({.texture = fakeTexture(1), .desc = backbufferDesc(), .writable = true});
        const Texture scene = graph.createTexture(texture("Scene"));
        const Texture unused = graph.createTexture(texture("Unused"));
        graph.addPass("Scene", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(scene); }, nothing());
        graph.addPass("Culled", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(unused); }, nothing());
        graph.addPass(
            "Present", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(scene, TextureUse::Sampled);
                pass.renderTarget(out);
            },
            nothing());
        graph.compile();
        CHECK(graph.dumpGraphviz() == "digraph \"Dot \\\"graph\\\"\" {\n"
                                      "  rankdir=LR;\n"
                                      "  p0 [shape=box, label=\"Scene\"];\n"
                                      "  p1 [shape=box, style=dashed, label=\"Culled (culled)\"];\n"
                                      "  p2 [shape=box, label=\"Present\"];\n"
                                      "  r0 [shape=ellipse, style=bold, label=\"Backbuffer\"];\n"
                                      "  r1 [shape=ellipse, label=\"Scene\"];\n"
                                      "  r2 [shape=ellipse, label=\"Unused\"];\n"
                                      "  p0 -> r1 [label=\"RenderTarget\"];\n"
                                      "  p1 -> r2 [label=\"RenderTarget\"];\n"
                                      "  r1 -> p2 [label=\"Sampled\"];\n"
                                      "  p2 -> r0 [label=\"RenderTarget\"];\n"
                                      "}\n");
    }
}
