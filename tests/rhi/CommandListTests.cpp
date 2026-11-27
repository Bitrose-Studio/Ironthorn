#include "ith/rhi/CommandList.h"

#include "ith/rhi/Device.h"
#include "ith/rhi/NullDevice.h"

#include "AssertCapture.h"
#include "RhiTestSupport.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream> // doctest prints std::string_view operands through it
#include <string>
#include <thread>
#include <vector>

using namespace ith::rhi;
using ith::Expected;
using ith::rhi::test::beginFrame;
using ith::rhi::test::endFrame;
using ith::rhi::test::makeBuffer;
using ith::rhi::test::makeComputePipeline;
using ith::rhi::test::makeDevice;
using ith::rhi::test::makeGraphicsPipeline;
using ith::rhi::test::makeTexture;

namespace {

// Whether GPU zones are compiled in (CommandList::beginGpuZone()).
#ifdef ITH_ENABLE_PROFILING
[[maybe_unused]] constexpr bool kProfiling = true;
#else
[[maybe_unused]] constexpr bool kProfiling = false;
#endif

struct DrawConstants {
    std::uint32_t vertices = 0;
    std::uint32_t material = 0;
    float scale = 1.0f;
    float bias = 0.0f;
};

} // namespace

TEST_SUITE("rhi.command_list") {
    TEST_CASE("a triangle frame records, submits and presents") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 320, .height = 200, .debugName = "Window"});
        REQUIRE(swapchain.hasValue());
        const std::array shaders{ShaderDesc{.stage = ShaderStage::Vertex, .code = ith::rhi::test::kShaderCode},
                                 ShaderDesc{.stage = ShaderStage::Pixel, .code = ith::rhi::test::kShaderCode}};
        const std::array targets{ColorTarget{.format = Format::B8G8R8A8Srgb}};
        const Expected<PipelineHandle, Error> pipeline =
            device.createGraphicsPipeline({.shaders = shaders, .colorTargets = targets, .debugName = "Triangle"});
        REQUIRE(pipeline.hasValue());

        const Frame frame = beginFrame(device, *swapchain);
        CommandList& list = device.openCommandList("Main pass");
        CHECK(list.debugName() == "Main pass");
        list.barrier(TextureBarrier{.texture = frame.backbuffer,
                                    .stagesAfter = PipelineStage::ColorOutput,
                                    .accessAfter = Access::RenderTargetWrite,
                                    .layoutBefore = TextureLayout::Undefined,
                                    .layoutAfter = TextureLayout::RenderTarget});
        const std::array colors{ColorAttachment{.texture = frame.backbuffer}};
        list.beginMarker("Triangle");
        list.beginRendering({.colors = colors});
        list.setPipeline(*pipeline);
        list.pushConstants(DrawConstants{.vertices = 3});
        list.draw(3);
        list.endRendering();
        list.endMarker();
        list.barrier(TextureBarrier{.texture = frame.backbuffer,
                                    .stagesBefore = PipelineStage::ColorOutput,
                                    .accessBefore = Access::RenderTargetWrite,
                                    .layoutBefore = TextureLayout::RenderTarget,
                                    .layoutAfter = TextureLayout::Present});
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
        CHECK(gpu.commandCount() == 9);
        CHECK(gpu.submitCount() == 1);
        CHECK(gpu.presentCount() == 1);
    }

    TEST_CASE("compute, copies and GPU-driven draws record") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const PipelineHandle compute = makeComputePipeline(device);
        const PipelineHandle graphics = makeGraphicsPipeline(device);
        const BufferHandle upload = makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload, 64 * 1024);
        const BufferHandle indices = makeBuffer(device, BufferUsage::Index | BufferUsage::CopyDst);
        const BufferHandle indirect =
            makeBuffer(device, BufferUsage::Indirect | BufferUsage::ShaderWrite, MemoryType::GpuOnly, 4096);
        const TextureHandle target = makeTexture(device, TextureUsage::RenderTarget);
        const TextureHandle albedo = makeTexture(device, TextureUsage::Sampled | TextureUsage::CopyDst);

        beginFrame(device);
        CommandList& list = device.openCommandList("Frame");
        list.copyBuffer(upload, 0, indices, 0, 256);
        list.copyBufferToTexture({.buffer = upload, .bufferOffset = 512, .rowPitch = 256, .texture = albedo});
        list.setPipeline(compute);
        list.dispatch(8, 8);
        list.dispatchIndirect(indirect, 0);
        const std::array colors{ColorAttachment{.texture = target, .load = LoadOp::Load}};
        list.beginRendering({.colors = colors});
        list.setPipeline(graphics);
        list.setViewport({.width = 64, .height = 64});
        list.setScissor({.width = 64, .height = 64});
        list.setIndexBuffer(indices, 0, IndexFormat::Uint32);
        list.drawIndexed(36, 10);
        list.drawIndexedIndirectCount(indirect, 16, indirect, 0, 100);
        list.endRendering();
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
        CHECK(gpu.commandCount() == 13);
    }

    TEST_CASE("lists submitted together go in one submission, and lists of later frames reuse the pool") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        beginFrame(device);
        CommandList& first = device.openCommandList("First");
        CommandList& second = device.openCommandList("Second");
        CHECK(&first != &second);
        const std::array<CommandList*, 2> lists{&first, &second};
        REQUIRE(device.submit(lists).hasValue());
        endFrame(device);
        CHECK(gpu.submitCount() == 1);

        ith::rhi::test::emptyFrame(device); // the other frame slot
        beginFrame(device);
        CommandList& reused = device.openCommandList("Reused");
        CHECK(&reused == &first);
        device.discard(reused);
        endFrame(device);
    }

    TEST_CASE("a list that could not start reports its error at submit and records nothing") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const PipelineHandle compute = makeComputePipeline(device);
        beginFrame(device);
        gpu.failNextCommandList();
        CommandList& list = device.openCommandList("Out of memory");
        list.setPipeline(compute);
        list.dispatch(1);
        const Expected<void, Error> submitted = device.submit(list);
        REQUIRE_FALSE(submitted.hasValue());
        CHECK(submitted.error().code == ErrorCode::OutOfMemory);
        CHECK_FALSE(device.isLost());
        CHECK(gpu.commandCount() == 0);
        CHECK(gpu.submitCount() == 0);
        endFrame(device);

        // The next list of that pool works again.
        ith::rhi::test::emptyFrame(device);
        beginFrame(device);
        CommandList& again = device.openCommandList("Works");
        again.setPipeline(compute);
        REQUIRE(device.submit(again).hasValue());
        endFrame(device);
        CHECK(gpu.commandCount() == 1);
    }

    TEST_CASE("endFrame discards lists that were left open") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device);
        beginFrame(device);
        device.openCommandList("Forgotten");
        device.destroy(buffer);
        const ith::test::AssertCapture capture;
        CHECK(device.endFrame().hasValue());
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 1);
#endif
        REQUIRE(device.waitIdle().hasValue());
        CHECK(gpu.liveObjectCount() == 0);
    }

    TEST_CASE("lists are opened and recorded on several threads at once") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const PipelineHandle compute = makeComputePipeline(device);
        constexpr int kThreads = 4;
        constexpr int kDispatches = 100;
        beginFrame(device);
        std::array<CommandList*, kThreads> lists{};
        std::vector<std::thread> threads;
        for (int thread = 0; thread < kThreads; ++thread) {
            threads.emplace_back([&device, &lists, compute, thread] {
                CommandList& list = device.openCommandList("Worker");
                list.setPipeline(compute);
                for (int dispatch = 0; dispatch < kDispatches; ++dispatch) {
                    list.pushConstants(DrawConstants{.material = static_cast<std::uint32_t>(dispatch)});
                    list.dispatch(1);
                }
                // Creating and destroying resources is thread-safe too.
                const Expected<BufferHandle, Error> scratch =
                    device.createBuffer({.size = 64, .usage = BufferUsage::ShaderWrite, .debugName = "Scratch"});
                if (scratch) {
                    device.destroy(*scratch);
                }
                lists[static_cast<std::size_t>(thread)] = &list;
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        REQUIRE(device.submit(lists).hasValue());
        endFrame(device);
        CHECK(gpu.commandCount() == kThreads * (1 + 2 * kDispatches));
        REQUIRE(device.waitIdle().hasValue());
        CHECK(gpu.liveObjectCount() == 1); // only the pipeline
    }
}

#ifdef ITH_ENABLE_ASSERTS
// The recording checks run in Debug and development builds only (ADR-0009 rule 12).
TEST_SUITE("rhi.command_list_checks") {
    TEST_CASE("draws and dispatches in the wrong state are asserted and dropped") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const PipelineHandle compute = makeComputePipeline(device);
        const PipelineHandle graphics = makeGraphicsPipeline(device);
        const TextureHandle target = makeTexture(device, TextureUsage::RenderTarget);
        beginFrame(device);
        CommandList& list = device.openCommandList("Misuse");
        const ith::test::AssertCapture capture;

        list.setPipeline(graphics);
        list.draw(3);     // outside rendering
        list.dispatch(1); // graphics pipeline bound
        const std::array colors{ColorAttachment{.texture = target}};
        list.beginRendering({.colors = colors});
        list.drawIndexed(3); // no index buffer
        list.setPipeline(compute);
        list.draw(3); // compute pipeline bound
        list.endRendering();
        list.endRendering(); // not rendering
        list.endMarker();    // no marker open
        CHECK(capture.count() == 6);
        CHECK(gpu.commandCount() == 4); // the two setPipeline, beginRendering and endRendering
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("an integer colour attachment clears only to all zeros") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const TextureHandle ids = makeTexture(device, TextureUsage::RenderTarget, Format::R32G32Uint);
        const TextureHandle color = makeTexture(device, TextureUsage::RenderTarget);
        beginFrame(device);
        CommandList& list = device.openCommandList("Integer clears");
        const ith::test::AssertCapture capture;
        // The default clear colour has alpha 1: refused, and nothing is recorded.
        const std::array defaulted{ColorAttachment{.texture = color}, ColorAttachment{.texture = ids}};
        list.beginRendering({.colors = defaulted});
        const std::array one{ColorAttachment{.texture = ids, .clearColor = {1.0f, 0.0f, 0.0f, 0.0f}}};
        list.beginRendering({.colors = one});
        CHECK(capture.count() == 2);
        CHECK(gpu.commandCount() == 0);
        // Zeros, and loads that don't clear, are fine; a normalized target clears to any colour.
        const std::array zero{ColorAttachment{.texture = color, .clearColor = {0.2f, 0.4f, 0.6f, 1.0f}},
                              ColorAttachment{.texture = ids, .clearColor = {0.0f, 0.0f, 0.0f, 0.0f}}};
        list.beginRendering({.colors = zero});
        list.endRendering();
        const std::array kept{
            ColorAttachment{.texture = ids, .load = LoadOp::Load, .clearColor = {5.0f, 0.0f, 0.0f, 1.0f}}};
        list.beginRendering({.colors = kept});
        list.endRendering();
        CHECK(capture.count() == 2);
        CHECK(gpu.commandCount() == 4);
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("stale handles and missing usages are asserted and dropped") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle stale = makeBuffer(device);
        device.destroy(stale);
        const BufferHandle plain = makeBuffer(device);
        const TextureHandle sampled = makeTexture(device);
        beginFrame(device);
        CommandList& list = device.openCommandList("Handles");
        const ith::test::AssertCapture capture;

        list.barrier(BufferBarrier{.buffer = stale});
        list.setPipeline(PipelineHandle{});
        list.setIndexBuffer(plain, 0, IndexFormat::Uint16);           // no Index usage
        const std::array colors{ColorAttachment{.texture = sampled}}; // no RenderTarget usage
        list.beginRendering({.colors = colors});
        list.barrier(
            TextureBarrier{.texture = sampled, .layoutAfter = TextureLayout::Present}); // not a swapchain image
        list.barrier(
            TextureBarrier{.texture = sampled, .layoutAfter = TextureLayout::ShaderRead, .range = {.baseMip = 1}});
        list.copyBuffer(plain, 0, plain, 0, 4); // same buffer
        CHECK(capture.count() == 7);
        CHECK(gpu.commandCount() == 0);
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("push constants are at most 128 bytes, in multiples of 4") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        beginFrame(device);
        CommandList& list = device.openCommandList("Constants");
        const std::array<std::byte, kMaxPushConstantSize> full{};
        const std::array<std::byte, kMaxPushConstantSize + 4> tooMany{};
        const std::array<std::byte, 6> unaligned{};
        const ith::test::AssertCapture capture;
        list.pushConstants(full);
        list.pushConstants(tooMany);
        list.pushConstants(unaligned);
        CHECK(capture.count() == 2);
        CHECK(gpu.commandCount() == 1);
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("copies are checked against their buffers and the texture copy alignment") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle source = makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload, 1024);
        const BufferHandle destination = makeBuffer(device, BufferUsage::CopyDst, MemoryType::GpuOnly, 512);
        const TextureHandle texture = makeTexture(device, TextureUsage::CopyDst, Format::R8G8B8A8Unorm, 4);
        beginFrame(device);
        CommandList& list = device.openCommandList("Copies");
        const ith::test::AssertCapture capture;
        list.copyBuffer(source, 600, destination, 0, 512);                                // past the source's end
        list.copyBuffer(destination, 0, source, 0, 4);                                    // wrong usages
        list.copyBufferToTexture({.buffer = source, .rowPitch = 16, .texture = texture}); // pitch not 256-aligned
        list.copyBufferToTexture(
            {.buffer = source, .bufferOffset = 256, .rowPitch = 256, .texture = texture}); // offset
        list.copyBufferToTexture(
            {.buffer = source, .bufferOffset = 512, .rowPitch = 256, .texture = texture});                // too long
        list.copyBufferToTexture({.buffer = source, .rowPitch = 256, .texture = texture, .mipLevel = 1}); // no mip 1
        CHECK(capture.count() == 6);
        list.copyBufferToTexture({.buffer = source, .rowPitch = 256, .texture = texture});
        list.copyBuffer(source, 512, destination, 0, 512);
        CHECK(capture.count() == 6);
        CHECK(gpu.commandCount() == 2);
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("texture readback copies are checked like uploads, with source and destination swapped") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle readback = makeBuffer(device, BufferUsage::CopyDst, MemoryType::Readback, 1024);
        const BufferHandle upload = makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload, 1024);
        const TextureHandle source = makeTexture(device, TextureUsage::CopySrc, Format::R8G8B8A8Unorm, 4);
        const TextureHandle uploadOnly = makeTexture(device, TextureUsage::CopyDst, Format::R8G8B8A8Unorm, 4);
        beginFrame(device);
        CommandList& list = device.openCommandList("Readback");
        const ith::test::AssertCapture capture;
        list.copyTextureToBuffer({.buffer = upload, .rowPitch = 256, .texture = source});       // buffer lacks CopyDst
        list.copyTextureToBuffer({.buffer = readback, .rowPitch = 256, .texture = uploadOnly}); // texture lacks CopySrc
        list.copyTextureToBuffer({.buffer = readback, .rowPitch = 16, .texture = source});      // pitch not 256-aligned
        list.copyTextureToBuffer(
            {.buffer = readback, .bufferOffset = 512, .rowPitch = 256, .texture = source});             // too long
        list.copyTextureToBuffer({.buffer = readback, .rowPitch = 256, .texture = source, .layer = 1}); // no layer 1
        CHECK(capture.count() == 5);
        CHECK(gpu.commandCount() == 0);
        list.copyTextureToBuffer({.buffer = readback, .rowPitch = 256, .texture = source});
        CHECK(capture.count() == 5);
        CHECK(gpu.commandCount() == 1);
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("a texel region lies inside its mip level, its data in the buffer, and depth copies are whole") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle source = makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload, 8192);
        const TextureHandle color = makeTexture(device, TextureUsage::CopyDst, Format::R8G8B8A8Unorm, 64);
        const TextureHandle depth =
            makeTexture(device, TextureUsage::DepthStencil | TextureUsage::CopyDst, Format::D32Float, 8);
        beginFrame(device);
        CommandList& list = device.openCommandList("Regions");
        const ith::test::AssertCapture capture;
        const auto copy = [&](TextureHandle texture, const TexelRegion& region, std::uint64_t offset = 0) {
            list.copyBufferToTexture(
                {.buffer = source, .bufferOffset = offset, .rowPitch = 256, .texture = texture, .region = region});
        };
        copy(color, {.x = 60, .width = 8});           // past the right edge
        copy(color, {.x = 64});                       // starts at the edge: nothing left
        copy(color, {.y = 10, .height = 60});         // past the bottom
        copy(color, {.z = 1});                        // a 2D texture has one slice
        copy(color, {.depth = 2});                    // likewise
        copy(color, {.width = 4, .height = 4}, 7680); // the rows run past the buffer
        copy(depth, {.width = 4});                    // part of a depth level
        copy(depth, {.x = 1});                        // likewise
        CHECK(capture.count() == 8);
        CHECK(gpu.commandCount() == 0);
        copy(color, {.x = 8, .y = 8, .width = 16, .height = 16});             // a rectangle inside
        copy(color, {.x = 32, .y = 32});                                      // from an offset to the end
        copy(color, {.x = 0, .y = 63, .width = 64, .height = 1, .depth = 1}); // the last row
        copy(depth, {});                                                      // a whole depth level
        CHECK(capture.count() == 8);
        CHECK(gpu.commandCount() == 4);
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("one R32G32Uint texel reads back into 8 bytes; its row must still be 256-aligned") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        // The viewport's pick: one texel of the id target into its 512-byte-aligned slot.
        const BufferHandle readback = makeBuffer(device, BufferUsage::CopyDst, MemoryType::Readback, 520);
        const TextureHandle ids =
            makeTexture(device, TextureUsage::RenderTarget | TextureUsage::CopySrc, Format::R32G32Uint, 64);
        beginFrame(device);
        CommandList& list = device.openCommandList("Pick");
        const ith::test::AssertCapture capture;
        const auto pick = [&](std::uint64_t offset, std::uint32_t rowPitch, std::uint32_t width = 1) {
            list.copyTextureToBuffer({.buffer = readback,
                                      .bufferOffset = offset,
                                      .rowPitch = rowPitch,
                                      .texture = ids,
                                      .region = {.x = 63, .y = 63, .width = width, .height = 1, .depth = 1}});
        };
        pick(512, 8);      // the row pitch is not a multiple of 256
        pick(256, 256);    // the offset is not a multiple of 512
        pick(516, 256);    // likewise, and 8 bytes would end past the buffer
        pick(512, 256, 2); // two texels: past the right edge
        CHECK(capture.count() == 4);
        CHECK(gpu.commandCount() == 0);
        pick(0, 256);
        pick(512, 256); // 8 bytes from 512 end inside the 520-byte buffer
        CHECK(capture.count() == 4);
        CHECK(gpu.commandCount() == 2);
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("recording into a submitted list and submitting it twice are asserted") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        beginFrame(device);
        CommandList& list = device.openCommandList("Once");
        REQUIRE(device.submit(list).hasValue());
        const ith::test::AssertCapture capture;
        list.dispatch(1);
        CHECK(device.submit(list).hasValue()); // nothing left to submit
        device.discard(list);
        CHECK(capture.count() == 3);
        CHECK(gpu.commandCount() == 0);
        endFrame(device);
    }

    TEST_CASE("submitting inside rendering or with an open marker is asserted") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const TextureHandle target = makeTexture(device, TextureUsage::RenderTarget);
        beginFrame(device);
        CommandList& list = device.openCommandList("Unfinished");
        const std::array colors{ColorAttachment{.texture = target}};
        list.beginMarker("Open");
        list.beginRendering({.colors = colors});
        const ith::test::AssertCapture capture;
        CHECK(device.submit(list).hasValue());
        CHECK(capture.count() == 2);
        endFrame(device);
    }

    TEST_CASE("GPU zones nest, span rendering and markers, and the null backend records nothing for them") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const TextureHandle target = makeTexture(device, TextureUsage::RenderTarget);
        beginFrame(device);
        CommandList& list = device.openCommandList("Zones");
        const ith::test::AssertCapture capture;
        const std::array colors{ColorAttachment{.texture = target}};
        list.beginGpuZone("Frame");
        {
            const std::string name = "Pass " + std::to_string(3); // a run-time name, gone after the call
            list.beginGpuZone(name);
        }
        list.beginMarker("Marker");
        list.beginRendering({.colors = colors});
        list.beginGpuZone("Inside rendering");
        list.endGpuZone();
        list.endRendering();
        list.endGpuZone(); // closes "Pass 3" inside the marker
        list.endMarker();
        list.beginGpuZone("");
        list.endGpuZone();
        list.endGpuZone();
        CHECK(capture.count() == 0);
        CHECK(gpu.commandCount() == 4); // the markers and the rendering
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("misused GPU zones are asserted and dropped with profiling, and compile to nothing without") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        beginFrame(device);
        CommandList& list = device.openCommandList("Misused zones");
        const ith::test::AssertCapture capture;
        list.endGpuZone(); // none open
        for (std::uint32_t depth = 0; depth <= kMaxGpuZoneDepth; ++depth) {
            list.beginGpuZone("Deep"); // the last one is too deep
        }
        CHECK(capture.count() == (kProfiling ? 2 : 0));
        for (std::uint32_t depth = 0; depth < kMaxGpuZoneDepth; ++depth) {
            list.endGpuZone(); // the dropped zone needs no end
        }
        CHECK(capture.count() == (kProfiling ? 2 : 0));
        list.beginGpuZone("Left open");
        CHECK(device.submit(list).hasValue());
        CHECK(capture.count() == (kProfiling ? 3 : 0));
        list.beginGpuZone("After submit"); // a closed list
        CHECK(capture.count() == (kProfiling ? 4 : 0));
        endFrame(device);

        // A list opened again starts without zones, though the last one was left open.
        beginFrame(device);
        CommandList& again = device.openCommandList("Again");
        again.endGpuZone();
        CHECK(capture.count() == (kProfiling ? 5 : 0));
        // Discarding with a zone open asserts too; the list is still discarded.
        again.beginGpuZone("Open at discard");
        device.discard(again);
        CHECK(capture.count() == (kProfiling ? 6 : 0));
        endFrame(device);
        CHECK(gpu.commandCount() == 0);
    }

    TEST_CASE("offsets and counts that reach past a buffer are asserted and dropped") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const PipelineHandle graphics = makeGraphicsPipeline(device);
        const PipelineHandle compute = makeComputePipeline(device);
        const BufferHandle indices = makeBuffer(device, BufferUsage::Index, MemoryType::GpuOnly, 64);
        const BufferHandle indirect = makeBuffer(device, BufferUsage::Indirect, MemoryType::GpuOnly, 64);
        const TextureHandle target = makeTexture(device, TextureUsage::RenderTarget);
        beginFrame(device);
        CommandList& list = device.openCommandList("Ranges");
        list.setPipeline(compute);
        const ith::test::AssertCapture capture;
        list.dispatchIndirect(indirect, 56); // 12 bytes of arguments don't fit after 56
        list.dispatchIndirect(indirect, 52);
        list.setIndexBuffer(indices, 64, IndexFormat::Uint32); // at the end
        list.setIndexBuffer(indices, 60, IndexFormat::Uint32);
        const std::array colors{ColorAttachment{.texture = target}};
        list.beginRendering({.colors = colors});
        list.setPipeline(graphics);
        list.drawIndexedIndirectCount(indirect, 0, indirect, 60, 4);            // 80 bytes of arguments
        list.drawIndexedIndirectCount(indirect, 0, indirect, 64, 3);            // the count is past the end
        list.drawIndexedIndirectCount(indirect, 0, indirect, 60, 0xFFFF'FFFFu); // no overflow
        list.drawIndexedIndirectCount(indirect, 0, indirect, 60, 3);
        list.endRendering();
        CHECK(capture.count() == 5);
        // setPipeline twice, the valid dispatchIndirect, setIndexBuffer and indirect draw,
        // beginRendering and endRendering.
        CHECK(gpu.commandCount() == 7);
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
    }

    TEST_CASE("a list opened outside a frame records nothing and leaves the frame's counts alone") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const PipelineHandle compute = makeComputePipeline(device);
        const ith::test::AssertCapture capture;
        CommandList& dead = device.openCommandList("Too early");
        CHECK(capture.count() == 1);
        dead.setPipeline(compute);
        dead.dispatch(1);
        device.discard(dead);
        CHECK(capture.count() == 1);
        CHECK(gpu.commandCount() == 0);

        beginFrame(device);
        const Expected<void, Error> submitted = device.submit(dead);
        REQUIRE_FALSE(submitted.hasValue());
        CHECK(submitted.error().code == ErrorCode::InvalidUsage);
        CommandList& live = device.openCommandList("In the frame");
        CHECK(&live != &dead);
        REQUIRE(device.submit(live).hasValue());
        CHECK(device.endFrame().hasValue()); // no list is counted as open
        CHECK(capture.count() == 1);
    }
}
#endif
