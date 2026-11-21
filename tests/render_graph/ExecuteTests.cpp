// Executing render graphs on the RHI's null backend: recording, groups, serial and parallel
// recording, the resource pool across frames, import write-back, clobbering and failures
// (ADR-0028 rule 22). Runs under ThreadSanitizer in CI.

#include "ith/jobs/JobSystem.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/NullDevice.h"

#include "RenderGraphTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <ostream> // doctest prints std::string operands through it
#include <string>
#include <vector>

using namespace ith;
using namespace ith::rg;
using ith::rg::test::beginFrame;
using ith::rg::test::endFrame;
using ith::rg::test::executeFrame;
using ith::rg::test::makeDevice;
using ith::rg::test::nothing;
using ith::rg::test::texture;
using rhi::Format;
using rhi::LoadOp;

namespace {

// Which list each pass recorded into, in the order they ran.
struct Recorded {
    std::mutex mutex;
    std::vector<std::string> passes;

    void add(const PassContext& context) {
        const std::lock_guard lock(mutex);
        passes.push_back(std::string(context.passName()) + " in '" + std::string(context.list().debugName()) + "'");
    }
};

// A chain of `count` compute passes into an imported buffer, each reading the previous one's
// output; the last writes the import.
void buildChain(RenderGraph& graph, Buffer output, int count, Recorded& recorded) {
    Texture previous = graph.createTexture(texture("Chain"));
    graph.addPass(
        "Pass 0", PassType::Compute, [&](PassBuilder& pass) { pass.write(previous, TextureUse::Storage); },
        [&recorded](PassContext& context) { recorded.add(context); });
    for (int i = 1; i < count; ++i) {
        const Texture next = graph.createTexture(texture("Chain"));
        const std::string name = "Pass " + std::to_string(i);
        graph.addPass(
            name, PassType::Compute,
            [&](PassBuilder& pass) {
                pass.read(previous, TextureUse::Sampled);
                pass.write(next, TextureUse::Storage);
                if (i + 1 == count) {
                    pass.write(output, BufferUse::CopyDst);
                }
            },
            [&recorded, previous, next](PassContext& context) {
                CHECK(context.sampledIndex(previous) != rhi::kInvalidDescriptorIndex);
                CHECK(context.storageIndex(next) != rhi::kInvalidDescriptorIndex);
                recorded.add(context);
            });
        previous = next;
    }
}

// Counts live copies of a capture, to see when the graph destroys execute callables.
struct LifetimeProbe {
    std::shared_ptr<int> token = std::make_shared<int>(0);
};

} // namespace

TEST_SUITE("render_graph.execute") {
    TEST_CASE("a frame on the backbuffer records each pass once, in order, and presents") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        Expected<rhi::SwapchainHandle, rhi::Error> swapchain =
            device.createSwapchain({.width = 64, .height = 64, .debugName = "Window"});
        REQUIRE(swapchain.hasValue());
        RenderGraph graph({.name = "Frame"});
        Recorded recorded;
        for (int frameIndex = 0; frameIndex < 3; ++frameIndex) {
            graph.reset();
            const rhi::Frame frame = beginFrame(device, *swapchain);
            const Texture backbuffer = graph.importBackbuffer(frame);
            const Texture depth = graph.createTexture(texture("Depth", Format::D32Float));
            const Texture unused = graph.createTexture(texture("Unused"));
            graph.addPass(
                "Scene", PassType::Raster,
                [&](PassBuilder& pass) {
                    pass.renderTarget(backbuffer);
                    pass.depth(depth);
                },
                [&recorded, depth](PassContext& context) {
                    CHECK_FALSE(context.texture(depth).isNull());
                    recorded.add(context);
                });
            graph.addPass(
                "Culled", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(unused); },
                [&recorded](PassContext& context) { recorded.add(context); });
            graph.compile();
            const std::uint64_t submits = gpu.submitCount();
            REQUIRE(graph.execute(pool).hasValue());
            CHECK(gpu.submitCount() == submits + 1);
            endFrame(device);
        }
        CHECK(recorded.passes == std::vector<std::string>{"Scene in 'Scene'", "Scene in 'Scene'", "Scene in 'Scene'"});
        CHECK(gpu.presentCount() == 3);
        // One depth buffer serves every frame.
        CHECK(pool.textureCount() == 1);
        graph.reset();
        REQUIRE(device.waitIdle().hasValue());
        device.destroy(*swapchain);
    }

    TEST_CASE("groups record into lists named after their first pass, and submit in order") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        Expected<rhi::BufferHandle, rhi::Error> output =
            device.createBuffer({.size = 256, .usage = rhi::BufferUsage::CopyDst, .debugName = "Output"});
        REQUIRE(output.hasValue());
        RenderGraph graph({.name = "Groups", .passesPerGroup = 2});
        Recorded recorded;
        const Buffer out =
            graph.importBuffer({.buffer = *output,
                                .desc = {.size = 256, .usage = rhi::BufferUsage::CopyDst, .debugName = "Output"},
                                .writable = true});
        buildChain(graph, out, 5, recorded);
        graph.compile();
        CHECK(graph.groupCount() == 3);
        const std::uint64_t submits = gpu.submitCount();
        executeFrame(device, graph, pool);
        CHECK(gpu.submitCount() == submits + 1); // one submit of three lists
        CHECK(recorded.passes == std::vector<std::string>{"Pass 0 in 'Pass 0'", "Pass 1 in 'Pass 0'",
                                                          "Pass 2 in 'Pass 2'", "Pass 3 in 'Pass 2'",
                                                          "Pass 4 in 'Pass 4'"});
        // Four chain textures alternate between two pooled ones; the last, never read, has another usage.
        CHECK(pool.textureCount() == 3);
        graph.reset();
        device.destroy(*output);
    }

    TEST_CASE("parallel recording on a job system records the same commands as serial recording") {
        const auto run = [](JobSystem* jobs, std::string& dump, std::vector<std::string>& passes) {
            rhi::NullGpu gpu;
            rhi::Device device = makeDevice(gpu);
            ResourcePool pool(device);
            Expected<rhi::BufferHandle, rhi::Error> output =
                device.createBuffer({.size = 256, .usage = rhi::BufferUsage::CopyDst, .debugName = "Output"});
            REQUIRE(output.hasValue());
            RenderGraph graph({.name = "Parallel", .passesPerGroup = 3});
            Recorded recorded;
            for (int frameIndex = 0; frameIndex < 3; ++frameIndex) {
                graph.reset();
                const Buffer out = graph.importBuffer(
                    {.buffer = *output,
                     .desc = {.size = 256, .usage = rhi::BufferUsage::CopyDst, .debugName = "Output"},
                     .writable = true});
                buildChain(graph, out, 16, recorded);
                graph.compile();
                executeFrame(device, graph, pool, jobs);
            }
            dump = graph.dump();
            {
                const std::lock_guard lock(recorded.mutex);
                passes = recorded.passes;
            }
            graph.reset();
            device.destroy(*output);
            REQUIRE(device.waitIdle().hasValue());
            return gpu.commandCount();
        };
        std::string serialDump;
        std::vector<std::string> serialPasses;
        const std::uint64_t serialCommands = run(nullptr, serialDump, serialPasses);

        JobSystem jobs({.foregroundWorkers = 4, .backgroundWorkers = 0});
        std::string parallelDump;
        std::vector<std::string> parallelPasses;
        const std::uint64_t parallelCommands = run(&jobs, parallelDump, parallelPasses);

        CHECK(serialCommands == parallelCommands);
        CHECK(serialDump == parallelDump);
        // Each pass recorded into the same list; only the order between lists may differ.
        std::sort(serialPasses.begin(), serialPasses.end());
        std::sort(parallelPasses.begin(), parallelPasses.end());
        CHECK(serialPasses == parallelPasses);
        CHECK(serialPasses.size() == 48);

        // Without workers, groups record in order on the calling thread.
        JobSystem noWorkers({.foregroundWorkers = 0, .backgroundWorkers = 0});
        std::string inlineDump;
        std::vector<std::string> inlinePasses;
        CHECK(run(&noWorkers, inlineDump, inlinePasses) == serialCommands);
    }

    TEST_CASE("an imported resource's final state is written back and starts the next frame") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        Expected<rhi::TextureHandle, rhi::Error> history =
            device.createTexture({.width = 64,
                                  .height = 64,
                                  .format = Format::R16G16B16A16Float,
                                  .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::RenderTarget,
                                  .debugName = "History"});
        REQUIRE(history.hasValue());
        ImportedState state;
        RenderGraph graph({.name = "History"});
        const auto frame = [&](bool write) {
            graph.reset();
            const Texture h =
                graph.importTexture({.texture = *history,
                                     .desc = {.width = 64,
                                              .height = 64,
                                              .format = Format::R16G16B16A16Float,
                                              .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::RenderTarget,
                                              .debugName = "History"},
                                     .state = &state,
                                     .writable = true});
            const Texture target = graph.createTexture(texture("Target"));
            if (write) {
                graph.addPass(
                    "Write history", PassType::Raster, [&](PassBuilder& pass) { pass.renderTarget(h, LoadOp::Clear); },
                    nothing());
            } else {
                graph.addPass(
                    "Read history", PassType::Raster,
                    [&](PassBuilder& pass) {
                        pass.read(h, TextureUse::Sampled, rhi::PipelineStage::PixelShader);
                        pass.renderTarget(target);
                    },
                    nothing(), PassFlags::NeverCull);
            }
            graph.compile();
            executeFrame(device, graph, pool);
        };
        frame(true);
        CHECK(state == ImportedState{.layout = rhi::TextureLayout::RenderTarget,
                                     .stages = rhi::PipelineStage::ColorOutput,
                                     .access = rhi::Access::RenderTargetWrite});
        frame(false);
        CHECK(graph.dump().find("  barrier texture 'History': RenderTarget -> ShaderRead; (ColorOutput, "
                                "RenderTargetWrite) -> (PixelShader, ShaderRead)\n") != std::string::npos);
        CHECK(state == ImportedState{.layout = rhi::TextureLayout::ShaderRead,
                                     .stages = rhi::PipelineStage::PixelShader,
                                     .access = rhi::Access::ShaderRead});
        frame(true);
        // A clear discards the contents, after the reads of the frame before.
        CHECK(graph.dump().find("  barrier texture 'History': Undefined -> RenderTarget; (PixelShader, None) -> "
                                "(ColorOutput, RenderTargetWrite)\n") != std::string::npos);
        graph.reset();
        device.destroy(*history);
    }

    TEST_CASE("a pooled resource's first barrier names its holder in the frame before") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        RenderGraph graph({.name = "Frames"});
        // A depth buffer, which is sampled by a compute pass when `sampled`.
        const auto frame = [&](bool sampled) {
            graph.reset();
            const Texture depth = graph.createTexture(texture("Depth", Format::D32Float));
            graph.addPass(
                "Depth", PassType::Raster, [&](PassBuilder& pass) { pass.depth(depth); }, nothing(),
                PassFlags::NeverCull);
            if (sampled) {
                const Texture out = graph.createTexture(texture("Out"));
                graph.addPass(
                    "Sample depth", PassType::Compute,
                    [&](PassBuilder& pass) {
                        pass.read(depth, TextureUse::DepthSampled);
                        pass.write(out, TextureUse::Storage);
                    },
                    nothing(), PassFlags::NeverCull);
            }
            graph.compile();
            CHECK(graph.dump().find("barrier texture 'Depth': Undefined -> DepthStencilWrite; (earlier frame) -> "
                                    "(DepthStencil, DepthStencilWrite)") != std::string::npos);
            executeFrame(device, graph, pool);
        };
        const auto firstBarrier = [&](std::string_view before) {
            const std::string expected = "  barrier texture 'Depth': Undefined -> DepthStencilWrite; (" +
                                         std::string(before) +
                                         ", earlier frame) -> (DepthStencil, DepthStencilWrite)\n";
            const std::string dump = graph.dump();
            CHECK_MESSAGE(dump.find(expected) != std::string::npos, dump);
        };
        // A fresh resource has no earlier use; the next frame's holder follows its depth writes.
        frame(false);
        firstBarrier("None, None");
        frame(false);
        firstBarrier("DepthStencil, DepthStencilWrite");
        CHECK(pool.textureCount() == 1);
        // Sampled too: another usage, so another pooled texture. Its next holder follows the
        // compute shader's reads: stages only.
        frame(true);
        firstBarrier("None, None");
        frame(true);
        firstBarrier("ComputeShader, None");
        CHECK(pool.textureCount() == 3);
        graph.reset();
    }

    TEST_CASE("pooled resources unused for maxUnusedFrames executes are destroyed") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device, {.maxUnusedFrames = 4});
        RenderGraph graph({.name = "Resize"});
        const auto frame = [&](std::uint32_t size) {
            graph.reset();
            graph.addPass(
                "Draw", PassType::Raster,
                [&](PassBuilder& pass) {
                    pass.renderTarget(graph.createTexture(texture("Color", Format::R8G8B8A8Unorm, size)));
                },
                nothing(), PassFlags::NeverCull);
            graph.compile();
            executeFrame(device, graph, pool);
        };
        frame(64);
        frame(64);
        CHECK(pool.textureCount() == 1);
        const std::uint32_t live = gpu.liveObjectCount();
        // A resize: the 64x64 entry stays for three more frames, and goes at the fourth.
        frame(32);
        CHECK(pool.textureCount() == 2);
        frame(32);
        frame(32);
        CHECK(pool.textureCount() == 2);
        frame(32);
        CHECK(pool.textureCount() == 1);
        // Deferred destruction releases it once the GPU is done.
        REQUIRE(device.waitIdle().hasValue());
        CHECK(gpu.liveObjectCount() == live);
        pool.clear();
        CHECK(pool.textureCount() == 0);
        graph.reset();
    }

    TEST_CASE("running out of memory fails execute before recording anything") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        RenderGraph graph({.name = "Memory"});
        graph.addPass(
            "Draw", PassType::Raster,
            [&](PassBuilder& pass) { pass.renderTarget(graph.createTexture(texture("Color"))); }, nothing(),
            PassFlags::NeverCull);
        graph.compile();
        beginFrame(device);
        gpu.failNextCreate();
        const std::uint64_t commands = gpu.commandCount();
        const Expected<void, rhi::Error> executed = graph.execute(pool);
        REQUIRE_FALSE(executed.hasValue());
        CHECK(executed.error().code == rhi::ErrorCode::OutOfMemory);
        CHECK(gpu.commandCount() == commands);
        endFrame(device);
        CHECK(pool.textureCount() == 0);
        graph.reset();
    }

    TEST_CASE("clobbering copies into every transient at its first use, and adds CopyDst") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        const auto build = [&](RenderGraph& graph) {
            const Texture color = graph.createTexture(
                {.width = 64, .height = 64, .mipLevels = 2, .format = Format::R8G8B8A8Unorm, .debugName = "Color"});
            const Texture depth = graph.createTexture(texture("Depth", Format::D32Float));
            const Buffer scratch = graph.createBuffer({.size = 3u << 20, .debugName = "Scratch"});
            graph.addPass(
                "Draw", PassType::Raster,
                [&](PassBuilder& pass) {
                    pass.renderTarget(color);
                    pass.depth(depth);
                },
                nothing());
            graph.addPass(
                "Use", PassType::Compute,
                [&](PassBuilder& pass) {
                    pass.read(color, TextureUse::Sampled);
                    pass.write(scratch, BufferUse::Storage);
                },
                nothing(), PassFlags::NeverCull);
            graph.compile();
        };
        RenderGraph plain({.name = "Plain"});
        build(plain);
        const std::uint64_t before = gpu.commandCount();
        executeFrame(device, plain, pool);
        const std::uint64_t plainCommands = gpu.commandCount() - before;

        RenderGraph clobbered({.name = "Clobbered", .clobberTransients = true});
        build(clobbered);
        const std::uint64_t start = gpu.commandCount();
        executeFrame(device, clobbered, pool);
        const std::uint64_t clobberedCommands = gpu.commandCount() - start;
#ifdef ITH_ENABLE_ASSERTS
        CHECK(clobbered.dump().find("usage Sampled|RenderTarget|CopyDst") != std::string::npos);
        // Per pass: one barrier batch to CopyDst. Copies: two mips of the color target, the depth
        // buffer, and three 1 MiB chunks of the scratch buffer.
        CHECK(clobberedCommands == plainCommands + 2 + 3 + 3);
#else
        CHECK(clobberedCommands == plainCommands);
#endif
        plain.reset();
        clobbered.reset();
    }

    TEST_CASE("execute callables are destroyed at reset, and culled ones never run") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        LifetimeProbe probe;
        std::atomic<int> runs{0};
        {
            RenderGraph graph({.name = "Lifetime"});
            graph.addPass(
                "Kept", PassType::Compute, [](PassBuilder&) {},
                [copy = probe, &runs](PassContext&) { runs.fetch_add(1); }, PassFlags::NeverCull);
            graph.addPass(
                "Culled", PassType::Compute, [](PassBuilder&) {},
                [copy = probe, &runs](PassContext&) { runs.fetch_add(100); });
            CHECK(probe.token.use_count() == 3);
            graph.compile();
            executeFrame(device, graph, pool);
            CHECK(runs.load() == 1);
            CHECK(probe.token.use_count() == 3);
            graph.reset();
            CHECK(probe.token.use_count() == 1);
            graph.addPass("Again", PassType::Compute, [](PassBuilder&) {}, [copy = probe](PassContext&) {});
            CHECK(probe.token.use_count() == 2);
        }
        // The graph's destructor resets it.
        CHECK(probe.token.use_count() == 1);
    }

    TEST_CASE("a graph without passes still moves the backbuffer to Present") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        Expected<rhi::SwapchainHandle, rhi::Error> swapchain =
            device.createSwapchain({.width = 64, .height = 64, .debugName = "Window"});
        REQUIRE(swapchain.hasValue());
        RenderGraph graph({.name = "Empty"});
        const rhi::Frame frame = beginFrame(device, *swapchain);
        static_cast<void>(graph.importBackbuffer(frame));
        graph.compile();
        CHECK(graph.groupCount() == 0);
        const std::uint64_t submits = gpu.submitCount();
        REQUIRE(graph.execute(pool).hasValue());
        CHECK(gpu.submitCount() == submits + 1);
        endFrame(device);
        graph.reset();
        REQUIRE(device.waitIdle().hasValue());
        device.destroy(*swapchain);
    }

    TEST_CASE("a failed submission writes back no imported state and no pooled last use") {
        rhi::NullGpu gpu;
        rhi::Device device = makeDevice(gpu);
        ResourcePool pool(device);
        Expected<rhi::TextureHandle, rhi::Error> target =
            device.createTexture({.width = 64,
                                  .height = 64,
                                  .format = Format::R8G8B8A8Unorm,
                                  .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
                                  .debugName = "Target"});
        REQUIRE(target.hasValue());
        const ImportedState before{.layout = rhi::TextureLayout::ShaderRead,
                                   .stages = rhi::PipelineStage::PixelShader,
                                   .access = rhi::Access::ShaderRead};
        ImportedState state = before;
        RenderGraph graph({.name = "Lost"});
        const Texture imported =
            graph.importTexture({.texture = *target,
                                 .desc = {.width = 64,
                                          .height = 64,
                                          .format = Format::R8G8B8A8Unorm,
                                          .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
                                          .debugName = "Target"},
                                 .state = &state,
                                 .writable = true});
        graph.addPass(
            "Draw", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.renderTarget(imported);
                pass.depth(graph.createTexture(texture("Depth", Format::D32Float)));
            },
            nothing());
        graph.compile();
        beginFrame(device);
        gpu.loseDevice("simulated hang");
        const Expected<void, rhi::Error> executed = graph.execute(pool);
        REQUIRE_FALSE(executed.hasValue());
        CHECK(executed.error().code == rhi::ErrorCode::DeviceLost);
        CHECK(state == before);
        static_cast<void>(device.endFrame());
        graph.reset();
    }
}
