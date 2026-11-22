// The Direct3D 12 backend on a GPU: WARP by default, so that CI runs it without a GPU, or the
// fastest hardware GPU with the environment variable ITH_RHI_TEST_ADAPTER=hardware. The debug
// layer is on outside Shipping (ITH_RHI_TEST_VALIDATION=off turns it off), and asserts on every
// error it reports.
//
// The shaders are compiled from Slang at run time when the shader compiler is built
// (IRONTHORN_BUILD_TOOLS); without it, the pipeline and draw tests are skipped.

#include "ith/rhi_d3d12/D3D12Device.h"

#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"

#ifdef ITH_TEST_HAS_SHADER_COMPILER
#include "ith/shader_compiler/ShaderCompiler.h"
#endif

#include "AssertCapture.h"

#include <doctest/doctest.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through it
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace ith::rhi;
using ith::Expected;

namespace {

// An environment variable of this suite; empty when unset. getenv_s: the C runtime deprecates
// getenv on Windows, the only platform of this suite.
std::string environment(const char* name) {
    std::array<char, 32> value{};
    std::size_t length = 0;
    if (getenv_s(&length, value.data(), value.size(), name) != 0 || length == 0) {
        return {};
    }
    return value.data();
}

AdapterPreference testAdapter() {
    return environment("ITH_RHI_TEST_ADAPTER") == "hardware" ? AdapterPreference::HighPerformance
                                                             : AdapterPreference::Software;
}

// The debug layer needs the Windows optional feature "Graphics Tools" besides the Agility SDK's
// d3d12SDKLayers.dll; ITH_RHI_TEST_VALIDATION=off runs the suite on a machine without it.
bool testValidation() {
    return environment("ITH_RHI_TEST_VALIDATION") != "off";
}

// doctest runs without exceptions here, so a failed REQUIRE or FAIL doesn't stop the test case
// (TestMain.cpp). The helpers below therefore never read the value of a failed ith::Expected:
// they record the failure and return a null handle, or a value the test case checks before it
// goes on.

// A device on the test adapter, or std::nullopt after recording the failure.
std::optional<Device> makeDevice() {
    Expected<Device, Error> device = createD3D12Device({.adapter = testAdapter(), .validation = testValidation()});
    if (!device) {
        FAIL(device.error().toString());
        return std::nullopt;
    }
    return std::move(device).value();
}

// A buffer, or the null handle after recording the failure.
BufferHandle makeBuffer(Device& device, BufferUsage usage, MemoryType memory, std::uint64_t size,
                        std::string_view name = "Test buffer") {
    Expected<BufferHandle, Error> buffer =
        device.createBuffer({.size = size, .usage = usage, .memory = memory, .debugName = name});
    if (!buffer) {
        FAIL(buffer.error().toString());
        return {};
    }
    return *buffer;
}

// A texture, or the null handle after recording the failure.
TextureHandle makeTexture(Device& device, const TextureDesc& desc) {
    Expected<TextureHandle, Error> texture = device.createTexture(desc);
    if (!texture) {
        FAIL(texture.error().toString());
        return {};
    }
    return *texture;
}

// Begins a frame without a swapchain; std::nullopt after recording the failure.
std::optional<Frame> beginFrame(Device& device) {
    Expected<std::optional<Frame>, Error> frame = device.beginFrame();
    if (!frame || !frame->has_value()) {
        FAIL("beginFrame() failed or drew nothing");
        return std::nullopt;
    }
    return **frame;
}

// Submits one list, ends the frame and waits for the GPU.
void finish(Device& device, CommandList& list) {
    const Expected<void, Error> submitted = device.submit(list);
    if (!submitted) {
        FAIL(submitted.error().toString());
    }
    REQUIRE(device.endFrame().hasValue());
    REQUIRE(device.waitIdle().hasValue());
}

// A texel of an R8G8B8A8 texture read back with a 256-byte row pitch.
std::array<int, 4> texel(std::span<const std::byte> bytes, std::uint32_t x, std::uint32_t y) {
    const std::size_t offset = std::size_t{y} * kTextureCopyRowAlignment + std::size_t{x} * 4;
    return {std::to_integer<int>(bytes[offset]), std::to_integer<int>(bytes[offset + 1]),
            std::to_integer<int>(bytes[offset + 2]), std::to_integer<int>(bytes[offset + 3])};
}

void checkTexel(std::span<const std::byte> bytes, std::uint32_t x, std::uint32_t y, std::array<int, 4> expected) {
    const std::array<int, 4> actual = texel(bytes, x, y);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        CHECK(std::abs(actual[channel] - expected[channel]) <= 1);
    }
}

// A 4x4 R8G8B8A8Unorm render target that can be read back.
TextureDesc targetDesc() {
    return {.width = 4,
            .height = 4,
            .format = Format::R8G8B8A8Unorm,
            .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
            .debugName = "Test target"};
}

// Moves a render target from Undefined to RenderTarget.
TextureBarrier toRenderTarget(TextureHandle texture) {
    return {.texture = texture,
            .stagesAfter = PipelineStage::ColorOutput,
            .accessAfter = Access::RenderTargetWrite,
            .layoutBefore = TextureLayout::Undefined,
            .layoutAfter = TextureLayout::RenderTarget};
}

// Moves a render target to CopySrc and copies it into a Readback buffer.
void readBack(CommandList& list, TextureHandle texture, BufferHandle readback) {
    list.barrier(TextureBarrier{.texture = texture,
                                .stagesBefore = PipelineStage::ColorOutput,
                                .accessBefore = Access::RenderTargetWrite,
                                .stagesAfter = PipelineStage::Copy,
                                .accessAfter = Access::CopyRead,
                                .layoutBefore = TextureLayout::RenderTarget,
                                .layoutAfter = TextureLayout::CopySrc});
    list.copyTextureToBuffer({.buffer = readback, .rowPitch = kTextureCopyRowAlignment, .texture = texture});
}

} // namespace

TEST_SUITE("rhi_d3d12.device") {
    TEST_CASE("a device reports Direct3D 12, DXIL and its adapter") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const Capabilities& capabilities = device->capabilities();
        MESSAGE("adapter: " << capabilities.adapter.name);
        CHECK(capabilities.backend == Backend::D3D12);
        CHECK(capabilities.shaderFormat == ShaderFormat::Dxil);
        CHECK_FALSE(capabilities.adapter.name.empty());
        CHECK(capabilities.adapter.software == (testAdapter() == AdapterPreference::Software));
        CHECK(capabilities.limits.resourceDescriptors == 262'144);
        CHECK(capabilities.limits.samplerDescriptors == 2'048);
        CHECK_FALSE(device->isLost());
    }

    TEST_CASE("createDevice without a choice creates a Direct3D 12 device") {
        Expected<Device, Error> device = createDevice({.adapter = testAdapter(), .validation = testValidation()});
        if (!device) {
            FAIL(device.error().toString());
            return;
        }
        CHECK(device->capabilities().backend == Backend::D3D12);
    }

    TEST_CASE("the high-performance adapter is a hardware GPU when there is one") {
        Expected<Device, Error> device = createD3D12Device({.validation = testValidation()});
        if (device) {
            MESSAGE("high-performance adapter: " << device->capabilities().adapter.name);
            CHECK_FALSE(device->capabilities().adapter.software);
        } else {
            // Hosted CI runners have no GPU.
            MESSAGE("no hardware GPU: " << device.error().toString());
            CHECK(device.error().code == ErrorCode::UnsupportedDevice);
        }
    }

    TEST_CASE("buffers, textures and samplers of every kind are created and destroyed") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const BufferHandle gpu = makeBuffer(*device,
                                            BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Index |
                                                BufferUsage::Indirect | BufferUsage::CopySrc | BufferUsage::CopyDst,
                                            MemoryType::GpuOnly, 1000);
        const BufferHandle upload =
            makeBuffer(*device, BufferUsage::ShaderRead | BufferUsage::CopySrc, MemoryType::Upload, 3);
        const BufferHandle readback = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, 4096);
        CHECK(device->shaderReadIndex(gpu) != kInvalidDescriptorIndex);
        CHECK(device->shaderWriteIndex(gpu) != device->shaderReadIndex(gpu));
        CHECK(device->mappedMemory(upload).size() == 3);
        CHECK(device->mappedMemory(readback).size() == 4096);
        device->mappedMemory(upload)[2] = std::byte{7}; // writable

        const std::array textures{
            makeTexture(*device, {.width = 64,
                                  .height = 32,
                                  .mipLevels = 3,
                                  .format = Format::R8G8B8A8Srgb,
                                  .usage = TextureUsage::Sampled | TextureUsage::RenderTarget | TextureUsage::CopyDst,
                                  .debugName = "Albedo"}),
            makeTexture(*device, {.width = 16,
                                  .height = 16,
                                  .layers = 4,
                                  .format = Format::R16G16B16A16Float,
                                  .usage = TextureUsage::Sampled | TextureUsage::Storage,
                                  .debugName = "Array"}),
            makeTexture(*device, {.dimension = TextureDimension::Texture3D,
                                  .width = 8,
                                  .height = 8,
                                  .depth = 8,
                                  .format = Format::R32Float,
                                  .usage = TextureUsage::Sampled | TextureUsage::Storage,
                                  .debugName = "Volume"}),
            makeTexture(*device, {.dimension = TextureDimension::TextureCube,
                                  .width = 16,
                                  .height = 16,
                                  .layers = 12,
                                  .format = Format::R11G11B10Float,
                                  .usage = TextureUsage::Sampled,
                                  .debugName = "Cubes"}),
            makeTexture(*device, {.width = 32,
                                  .height = 32,
                                  .format = Format::D32Float,
                                  .usage = TextureUsage::DepthStencil | TextureUsage::Sampled,
                                  .debugName = "Depth"}),
            makeTexture(*device, {.width = 32,
                                  .height = 32,
                                  .format = Format::D32FloatS8Uint,
                                  .usage = TextureUsage::DepthStencil,
                                  .debugName = "Depth stencil"}),
        };
        CHECK(device->sampledIndex(textures[0]) != kInvalidDescriptorIndex);
        CHECK(device->storageIndex(textures[1]) != kInvalidDescriptorIndex);

        Expected<SamplerHandle, Error> linear = device->createSampler({.maxAnisotropy = 8.0f, .debugName = "Aniso"});
        Expected<SamplerHandle, Error> shadow =
            device->createSampler({.addressU = AddressMode::ClampToBorder, .compare = true, .debugName = "Shadow"});
        if (!linear || !shadow) {
            FAIL("creating a sampler failed");
            return;
        }
        CHECK(device->samplerIndex(*linear) != device->samplerIndex(*shadow));

        for (const TextureHandle texture : textures) {
            device->destroy(texture);
        }
        device->destroy(gpu);
        device->destroy(upload);
        device->destroy(readback);
        device->destroy(*linear);
        device->destroy(*shadow);
        CHECK(device->pendingDestructionCount() == 11);
        REQUIRE(device->waitIdle().hasValue());
        CHECK(device->pendingDestructionCount() == 0);
    }

    TEST_CASE("bytes that are not DXIL are refused as invalid shaders") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const std::array<std::byte, 64> notDxil{};
        const Expected<PipelineHandle, Error> pipeline = device->createComputePipeline(
            {.shader = {.stage = ShaderStage::Compute, .code = notDxil}, .debugName = "Not DXIL"});
        REQUIRE_FALSE(pipeline.hasValue());
        CHECK(pipeline.error().code == ErrorCode::InvalidShader);
        CHECK(pipeline.error().message.find("Not DXIL") != std::string::npos);
    }
}

TEST_SUITE("rhi_d3d12.frames") {
    TEST_CASE("an object destroyed while a list uses it is released only after the GPU is done") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const BufferHandle source = makeBuffer(*device, BufferUsage::CopySrc, MemoryType::Upload, 256);
        const BufferHandle destination = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::GpuOnly, 256);

        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Uses the buffers");
        list.copyBuffer(source, 0, destination, 0, 256);
        device->destroy(source);
        device->destroy(destination);
        CHECK_FALSE(device->isValid(source));
        REQUIRE(device->submit(list).hasValue());
        REQUIRE(device->endFrame().hasValue());
        CHECK(device->pendingDestructionCount() == 2);

        // Two frames later the frame that used them has finished on the GPU.
        for (int frame = 0; frame < static_cast<int>(kFramesInFlight) + 1; ++frame) {
            if (!beginFrame(*device)) {
                return;
            }
            REQUIRE(device->endFrame().hasValue());
        }
        CHECK(device->pendingDestructionCount() == 0);
    }

    TEST_CASE("many frames with lists recorded on the pooled allocators run back to back") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const BufferHandle source = makeBuffer(*device, BufferUsage::CopySrc, MemoryType::Upload, 1024);
        const BufferHandle destination = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::GpuOnly, 1024);
        for (int frame = 0; frame < 20; ++frame) {
            if (!beginFrame(*device)) {
                return;
            }
            CommandList& first = device->openCommandList("First");
            CommandList& second = device->openCommandList("Second");
            first.beginMarker("Copy");
            first.copyBuffer(source, 0, destination, 0, 512);
            first.endMarker();
            second.copyBuffer(source, 512, destination, 512, 512);
            if (frame % 3 == 0) {
                device->discard(second);
                REQUIRE(device->submit(first).hasValue());
            } else {
                const std::array<CommandList*, 2> lists{&first, &second};
                REQUIRE(device->submit(lists).hasValue());
            }
            REQUIRE(device->endFrame().hasValue());
        }
        CHECK(device->frameNumber() == 20);
        REQUIRE(device->waitIdle().hasValue());
    }
}

TEST_SUITE("rhi_d3d12.commands") {
    TEST_CASE("a cleared render target reads back with the clear color") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const TextureHandle target = makeTexture(*device, targetDesc());
        const BufferHandle readback = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, 1024);

        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Clear");
        list.barrier(toRenderTarget(target));
        const std::array colors{ColorAttachment{.texture = target, .clearColor = {0.25f, 0.5f, 0.75f, 1.0f}}};
        list.beginRendering({.colors = colors});
        list.endRendering();
        readBack(list, target, readback);
        finish(*device, list);

        const std::span<const std::byte> pixels = device->mappedMemory(readback);
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                checkTexel(pixels, x, y, {64, 128, 191, 255});
            }
        }
    }

    TEST_CASE("data survives a round trip through a GPU buffer and a texture") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        constexpr std::uint64_t kSize = 4 * kTextureCopyRowAlignment; // a 4x4 RGBA8 texture, rows 256 bytes apart
        const BufferHandle upload = makeBuffer(*device, BufferUsage::CopySrc, MemoryType::Upload, kSize);
        const BufferHandle middle =
            makeBuffer(*device, BufferUsage::CopySrc | BufferUsage::CopyDst, MemoryType::GpuOnly, kSize);
        const BufferHandle bufferReadback = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, kSize);
        const BufferHandle textureReadback = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, kSize);
        const TextureHandle texture = makeTexture(*device, {.width = 4,
                                                            .height = 4,
                                                            .format = Format::R8G8B8A8Unorm,
                                                            .usage = TextureUsage::CopySrc | TextureUsage::CopyDst,
                                                            .debugName = "Round trip"});
        const std::span<std::byte> source = device->mappedMemory(upload);
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] = static_cast<std::byte>(index * 7 + 3);
        }

        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Round trip");
        list.copyBuffer(upload, 0, middle, 0, kSize);
        list.barrier(BufferBarrier{.buffer = middle,
                                   .stagesBefore = PipelineStage::Copy,
                                   .accessBefore = Access::CopyWrite,
                                   .stagesAfter = PipelineStage::Copy,
                                   .accessAfter = Access::CopyRead});
        list.copyBuffer(middle, 0, bufferReadback, 0, kSize);
        list.barrier(TextureBarrier{.texture = texture,
                                    .stagesAfter = PipelineStage::Copy,
                                    .accessAfter = Access::CopyWrite,
                                    .layoutBefore = TextureLayout::Undefined,
                                    .layoutAfter = TextureLayout::CopyDst});
        list.copyBufferToTexture({.buffer = upload, .rowPitch = kTextureCopyRowAlignment, .texture = texture});
        list.barrier(TextureBarrier{.texture = texture,
                                    .stagesBefore = PipelineStage::Copy,
                                    .accessBefore = Access::CopyWrite,
                                    .stagesAfter = PipelineStage::Copy,
                                    .accessAfter = Access::CopyRead,
                                    .layoutBefore = TextureLayout::CopyDst,
                                    .layoutAfter = TextureLayout::CopySrc});
        list.copyTextureToBuffer({.buffer = textureReadback, .rowPitch = kTextureCopyRowAlignment, .texture = texture});
        finish(*device, list);

        const std::span<const std::byte> viaBuffer = device->mappedMemory(bufferReadback);
        CHECK(std::memcmp(viaBuffer.data(), source.data(), kSize) == 0);
        // Only the 16 bytes of each row that hold texels go through the texture.
        const std::span<const std::byte> viaTexture = device->mappedMemory(textureReadback);
        for (std::size_t row = 0; row < 4; ++row) {
            const std::size_t offset = row * kTextureCopyRowAlignment;
            CHECK(std::memcmp(viaTexture.data() + offset, source.data() + offset, 16) == 0);
        }
    }

    TEST_CASE("a removed device reports device loss with the removal reason") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const BufferHandle buffer = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, 256);
        if (!beginFrame(*device)) {
            return;
        }
        REQUIRE(device->endFrame().hasValue());

        // The debug layer may report the removal as an error, which the backend asserts.
        const ith::test::AssertCapture capture;
        removeD3D12DeviceForTesting(*device);
        Expected<std::optional<Frame>, Error> frame = device->beginFrame();
        if (frame && frame->has_value()) {
            // The wait finished before the removal was seen; the next submission reports it.
            CommandList& list = device->openCommandList("After removal");
            const Expected<void, Error> submitted = device->submit(list);
            REQUIRE_FALSE(submitted.hasValue());
            CHECK(submitted.error().code == ErrorCode::DeviceLost);
            static_cast<void>(device->endFrame());
        } else {
            REQUIRE_FALSE(frame.hasValue());
            CHECK(frame.error().code == ErrorCode::DeviceLost);
            MESSAGE(frame.error().toString());
            CHECK(frame.error().message.find("Direct3D 12 device was lost") != std::string::npos);
        }
        CHECK(device->isLost());
        const Expected<BufferHandle, Error> another = device->createBuffer(
            {.size = 4, .usage = BufferUsage::CopyDst, .memory = MemoryType::Readback, .debugName = "Late"});
        REQUIRE_FALSE(another.hasValue());
        CHECK(another.error().code == ErrorCode::DeviceLost);
        device->destroy(buffer);
    }
}

TEST_SUITE("rhi_d3d12.failures") {
    TEST_CASE("a failed timeline signal in submit removes the device and nothing waits forever") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Before the failure");
        failNextD3D12SignalForTesting(*device);
        const ith::test::AssertCapture capture; // the debug layer may report the removal
        const Expected<void, Error> submitted = device->submit(list);
        REQUIRE_FALSE(submitted.hasValue());
        CHECK(submitted.error().code == ErrorCode::DeviceLost);
        CHECK(submitted.error().message.find("signaling the graphics timeline failed") != std::string::npos);
        static_cast<void>(device->endFrame());
        CHECK(device->isLost());
        const Expected<void, Error> idle = device->waitIdle();
        REQUIRE_FALSE(idle.hasValue());
        CHECK(idle.error().code == ErrorCode::DeviceLost);
    }

    TEST_CASE("a failed timeline signal at the end of a frame reports device loss") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        if (!beginFrame(*device)) {
            return;
        }
        failNextD3D12SignalForTesting(*device);
        const ith::test::AssertCapture capture;
        const Expected<void, Error> ended = device->endFrame();
        REQUIRE_FALSE(ended.hasValue());
        CHECK(ended.error().code == ErrorCode::DeviceLost);
        CHECK(device->isLost());
    }
}

namespace {

// A render target first used without a clear (its first barrier discards it), then cleared in a
// second rendering and read back.
void discardThenClear(Device& device) {
    const TextureHandle target = makeTexture(device, targetDesc());
    const BufferHandle readback = makeBuffer(device, BufferUsage::CopyDst, MemoryType::Readback, 1024);
    if (!beginFrame(device)) {
        return;
    }
    CommandList& list = device.openCommandList("Discard then clear");
    list.barrier(toRenderTarget(target));
    const std::array untouched{ColorAttachment{.texture = target, .load = LoadOp::DontCare}};
    list.beginRendering({.colors = untouched});
    list.endRendering();
    const std::array cleared{ColorAttachment{.texture = target, .clearColor = {1.0f, 0.0f, 0.0f, 1.0f}}};
    list.beginRendering({.colors = cleared});
    list.endRendering();
    readBack(list, target, readback);
    finish(device, list);
    checkTexel(device.mappedMemory(readback), 0, 0, {255, 0, 0, 255});
    checkTexel(device.mappedMemory(readback), 3, 3, {255, 0, 0, 255});
}

// Uploads a pattern into one mip level and layer of a texture, copies it back out and compares
// the bytes of every row. `rows` counts the rows of all slices of a 3D texture.
void copyRoundTrip(Device& device, const TextureDesc& desc, std::uint32_t mip, std::uint32_t layer,
                   std::uint32_t rowBytes, std::uint32_t rows) {
    CAPTURE(desc.debugName);
    const std::uint64_t size = std::uint64_t{kTextureCopyRowAlignment} * rows;
    const BufferHandle upload = makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload, size);
    const BufferHandle readback = makeBuffer(device, BufferUsage::CopyDst, MemoryType::Readback, size);
    const TextureHandle texture = makeTexture(device, desc);
    // Floats from 0 to 1, valid in every format here, depth included.
    const std::span<std::byte> source = device.mappedMemory(upload);
    for (std::size_t word = 0; word < source.size() / 4; ++word) {
        const float value = static_cast<float>(word % 1000) / 1000.0f;
        std::memcpy(source.data() + word * 4, &value, sizeof(value));
    }

    if (!beginFrame(device)) {
        return;
    }
    CommandList& list = device.openCommandList("Texture copies");
    list.barrier(TextureBarrier{.texture = texture,
                                .stagesAfter = PipelineStage::Copy,
                                .accessAfter = Access::CopyWrite,
                                .layoutBefore = TextureLayout::Undefined,
                                .layoutAfter = TextureLayout::CopyDst});
    const BufferTextureCopy in{
        .buffer = upload, .rowPitch = kTextureCopyRowAlignment, .texture = texture, .mipLevel = mip, .layer = layer};
    list.copyBufferToTexture(in);
    list.barrier(TextureBarrier{.texture = texture,
                                .stagesBefore = PipelineStage::Copy,
                                .accessBefore = Access::CopyWrite,
                                .stagesAfter = PipelineStage::Copy,
                                .accessAfter = Access::CopyRead,
                                .layoutBefore = TextureLayout::CopyDst,
                                .layoutAfter = TextureLayout::CopySrc});
    BufferTextureCopy out = in;
    out.buffer = readback;
    list.copyTextureToBuffer(out);
    finish(device, list);

    const std::span<const std::byte> result = device.mappedMemory(readback);
    for (std::uint32_t row = 0; row < rows; ++row) {
        CAPTURE(row);
        const std::size_t offset = std::size_t{row} * kTextureCopyRowAlignment;
        CHECK(std::memcmp(result.data() + offset, source.data() + offset, rowBytes) == 0);
    }
}

// Fills a texture through a whole-level upload, overwrites a rectangle of it with a second,
// partial upload, and reads back the whole level and the rectangle alone: only the rectangle
// changes, and the partial readback holds just its texels.
void partialCopies(Device& device) {
    constexpr std::uint32_t kSize = 16;
    constexpr TexelRegion kRect{.x = 5, .y = 6, .width = 4, .height = 3};
    constexpr std::uint64_t kRectOffset = std::uint64_t{kTextureCopyRowAlignment} * kSize;
    constexpr std::uint64_t kBufferSize = kRectOffset * 2;
    const BufferHandle upload = makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload, kBufferSize);
    const BufferHandle readback = makeBuffer(device, BufferUsage::CopyDst, MemoryType::Readback, kBufferSize);
    const TextureHandle texture = makeTexture(device, {.width = kSize,
                                                       .height = kSize,
                                                       .format = Format::R8G8B8A8Unorm,
                                                       .usage = TextureUsage::CopySrc | TextureUsage::CopyDst,
                                                       .debugName = "Partial updates"});
    const std::span<std::byte> source = device.mappedMemory(upload);
    std::memset(source.data(), 0x11, static_cast<std::size_t>(kRectOffset));
    std::memset(source.data() + kRectOffset, 0xAB, static_cast<std::size_t>(kRectOffset));

    if (!beginFrame(device)) {
        return;
    }
    CommandList& list = device.openCommandList("Partial copies");
    list.barrier(TextureBarrier{.texture = texture,
                                .stagesAfter = PipelineStage::Copy,
                                .accessAfter = Access::CopyWrite,
                                .layoutBefore = TextureLayout::Undefined,
                                .layoutAfter = TextureLayout::CopyDst});
    list.copyBufferToTexture({.buffer = upload, .rowPitch = kTextureCopyRowAlignment, .texture = texture});
    list.barrier(TextureBarrier{.texture = texture,
                                .stagesBefore = PipelineStage::Copy,
                                .accessBefore = Access::CopyWrite,
                                .stagesAfter = PipelineStage::Copy,
                                .accessAfter = Access::CopyWrite,
                                .layoutBefore = TextureLayout::CopyDst,
                                .layoutAfter = TextureLayout::CopyDst});
    list.copyBufferToTexture({.buffer = upload,
                              .bufferOffset = kRectOffset,
                              .rowPitch = kTextureCopyRowAlignment,
                              .texture = texture,
                              .region = kRect});
    list.barrier(TextureBarrier{.texture = texture,
                                .stagesBefore = PipelineStage::Copy,
                                .accessBefore = Access::CopyWrite,
                                .stagesAfter = PipelineStage::Copy,
                                .accessAfter = Access::CopyRead,
                                .layoutBefore = TextureLayout::CopyDst,
                                .layoutAfter = TextureLayout::CopySrc});
    list.copyTextureToBuffer({.buffer = readback, .rowPitch = kTextureCopyRowAlignment, .texture = texture});
    list.copyTextureToBuffer({.buffer = readback,
                              .bufferOffset = kRectOffset,
                              .rowPitch = kTextureCopyRowAlignment,
                              .texture = texture,
                              .region = kRect});
    finish(device, list);

    const std::span<const std::byte> result = device.mappedMemory(readback);
    const auto byteAt = [&](std::uint64_t offset) { return std::to_integer<int>(result[offset]); };
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            const bool inside = x >= kRect.x && x < kRect.x + kRect.width && y >= kRect.y && y < kRect.y + kRect.height;
            const std::uint64_t texel = std::uint64_t{y} * kTextureCopyRowAlignment + std::uint64_t{x} * 4;
            for (std::uint64_t channel = 0; channel < 4; ++channel) {
                CHECK(byteAt(texel + channel) == (inside ? 0xAB : 0x11));
            }
        }
    }
    for (std::uint32_t row = 0; row < kRect.height; ++row) {
        CAPTURE(row);
        for (std::uint64_t byte = 0; byte < kRect.width * 4; ++byte) {
            CHECK(byteAt(kRectOffset + std::uint64_t{row} * kTextureCopyRowAlignment + byte) == 0xAB);
        }
    }
}

// A window that is never shown, for swapchain tests; its hwnd is null where there is no desktop.
struct HiddenWindow {
    HiddenWindow() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"IronthornTestWindow";
        RegisterClassExW(&windowClass); // fails harmlessly when already registered
        hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"Ironthorn test", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256,
                               nullptr, nullptr, instance, nullptr);
        handle = ith::Win32WindowHandle{.hwnd = hwnd, .hinstance = instance};
    }

    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;
    HiddenWindow(HiddenWindow&&) = delete;
    HiddenWindow& operator=(HiddenWindow&&) = delete;

    ~HiddenWindow() {
        if (hwnd != nullptr) {
            DestroyWindow(hwnd);
        }
    }

    HWND hwnd = nullptr;
    ith::Win32WindowHandle handle;
};

// Clears and presents one frame of the swapchain; returns the frame, or nothing when the frame
// was skipped (zero size).
std::optional<Frame> presentFrame(Device& device, SwapchainHandle swapchain) {
    Expected<std::optional<Frame>, Error> frame = device.beginFrame(swapchain);
    if (!frame) {
        FAIL(frame.error().toString());
        return std::nullopt;
    }
    if (!frame->has_value()) {
        return std::nullopt;
    }
    const TextureHandle image = (*frame)->backbuffer;
    CommandList& list = device.openCommandList("Present");
    list.barrier(toRenderTarget(image));
    const std::array colors{ColorAttachment{.texture = image, .clearColor = {0.0f, 0.5f, 1.0f, 1.0f}}};
    list.beginRendering({.colors = colors});
    list.endRendering();
    list.barrier(TextureBarrier{.texture = image,
                                .stagesBefore = PipelineStage::ColorOutput,
                                .accessBefore = Access::RenderTargetWrite,
                                .layoutBefore = TextureLayout::RenderTarget,
                                .layoutAfter = TextureLayout::Present});
    REQUIRE(device.submit(list).hasValue());
    const Expected<void, Error> ended = device.endFrame();
    if (!ended) {
        FAIL(ended.error().toString());
    }
    return *frame;
}

} // namespace

TEST_SUITE("rhi_d3d12.textures") {
    TEST_CASE("a render target first used without a clear is discarded, and a later clear reads back") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        discardThenClear(*device);
    }

    // Regression: a barrier out of Undefined that names the earlier writes it follows, as Vulkan
    // needs for a depth buffer reused every frame, was refused by the debug layer.
    TEST_CASE("a target discarded after earlier writes, naming them, is cleared again") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const TextureHandle target = makeTexture(*device, targetDesc());
        const BufferHandle readback = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, 1024);
        if (!beginFrame(*device)) {
            return;
        }
        CommandList& first = device->openCommandList("First clear");
        first.barrier(toRenderTarget(target));
        const std::array green{ColorAttachment{.texture = target, .clearColor = {0.0f, 1.0f, 0.0f, 1.0f}}};
        first.beginRendering({.colors = green});
        first.endRendering();
        REQUIRE(device->submit(first).hasValue());
        CommandList& second = device->openCommandList("Discard and clear");
        second.barrier(TextureBarrier{.texture = target,
                                      .stagesBefore = PipelineStage::ColorOutput,
                                      .accessBefore = Access::RenderTargetWrite,
                                      .stagesAfter = PipelineStage::ColorOutput,
                                      .accessAfter = Access::RenderTargetWrite,
                                      .layoutBefore = TextureLayout::Undefined,
                                      .layoutAfter = TextureLayout::RenderTarget});
        const std::array red{ColorAttachment{.texture = target, .clearColor = {1.0f, 0.0f, 0.0f, 1.0f}}};
        second.beginRendering({.colors = red});
        second.endRendering();
        readBack(second, target, readback);
        finish(*device, second);
        checkTexel(device->mappedMemory(readback), 0, 0, {255, 0, 0, 255});
    }

    TEST_CASE("the discard and clear pass GPU-based validation") {
        if (!testValidation()) {
            MESSAGE("skipped: validation is off (ITH_RHI_TEST_VALIDATION=off)");
            return;
        }
        Expected<Device, Error> device =
            createD3D12Device({.adapter = testAdapter(), .validation = true, .gpuBasedValidation = true});
        if (!device) {
            FAIL(device.error().toString());
            return;
        }
        discardThenClear(*device);
    }

    TEST_CASE("a partial upload changes only its rectangle, and a partial readback reads only it") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        partialCopies(*device);
    }

    TEST_CASE("copies reach mip levels and layers beyond the first, 3D slices and both depth formats") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        copyRoundTrip(*device,
                      {.width = 16,
                       .height = 16,
                       .layers = 4,
                       .mipLevels = 3,
                       .format = Format::R8G8B8A8Unorm,
                       .usage = TextureUsage::CopySrc | TextureUsage::CopyDst,
                       .debugName = "Array, mip 1, layer 2"},
                      1, 2, 8 * 4, 8);
        copyRoundTrip(*device,
                      {.dimension = TextureDimension::Texture3D,
                       .width = 4,
                       .height = 4,
                       .depth = 4,
                       .format = Format::R32Float,
                       .usage = TextureUsage::CopySrc | TextureUsage::CopyDst,
                       .debugName = "Volume"},
                      0, 0, 4 * 4, 4 * 4);
        copyRoundTrip(*device,
                      {.dimension = TextureDimension::Texture3D,
                       .width = 4,
                       .height = 4,
                       .depth = 4,
                       .mipLevels = 2,
                       .format = Format::R32Float,
                       .usage = TextureUsage::CopySrc | TextureUsage::CopyDst,
                       .debugName = "Volume, mip 1"},
                      1, 0, 2 * 4, 2 * 2);
        copyRoundTrip(*device,
                      {.width = 8,
                       .height = 8,
                       .format = Format::D32Float,
                       .usage = TextureUsage::DepthStencil | TextureUsage::CopySrc | TextureUsage::CopyDst,
                       .debugName = "Depth"},
                      0, 0, 8 * 4, 8);
        copyRoundTrip(*device,
                      {.width = 8,
                       .height = 8,
                       .format = Format::D32FloatS8Uint,
                       .usage = TextureUsage::DepthStencil | TextureUsage::CopySrc | TextureUsage::CopyDst,
                       .debugName = "Depth plane of depth-stencil"},
                      0, 0, 8 * 4, 8);
    }
}

TEST_SUITE("rhi_d3d12.threads") {
    TEST_CASE("lists recorded on several threads while other threads create and destroy resources") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        constexpr std::size_t kRecorders = 4;
        std::array<BufferHandle, kRecorders> sources{};
        std::array<BufferHandle, kRecorders> destinations{};
        for (std::size_t index = 0; index < kRecorders; ++index) {
            sources[index] = makeBuffer(*device, BufferUsage::CopySrc, MemoryType::Upload, 4096);
            destinations[index] = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::GpuOnly, 4096);
        }

        std::atomic<bool> stop{false};
        std::atomic<int> churned{0};
        std::vector<std::thread> churners;
        for (int thread = 0; thread < 2; ++thread) {
            churners.emplace_back([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    const Expected<BufferHandle, Error> buffer = device->createBuffer(
                        {.size = 1024, .usage = BufferUsage::ShaderRead, .debugName = "Churned buffer"});
                    const Expected<TextureHandle, Error> texture =
                        device->createTexture({.width = 16,
                                               .height = 16,
                                               .format = Format::R8G8B8A8Unorm,
                                               .usage = TextureUsage::Sampled,
                                               .debugName = "Churned texture"});
                    if (buffer) {
                        device->destroy(*buffer);
                    }
                    if (texture) {
                        device->destroy(*texture);
                    }
                    churned.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (int frame = 0; frame < 10; ++frame) {
            if (!beginFrame(*device)) {
                return;
            }
            std::array<CommandList*, kRecorders> lists{};
            std::vector<std::thread> recorders;
            for (std::size_t index = 0; index < kRecorders; ++index) {
                recorders.emplace_back([&, index] {
                    CommandList& list = device->openCommandList("Recorder");
                    list.beginMarker("Copies");
                    for (std::uint64_t offset = 0; offset < 4096; offset += 512) {
                        list.copyBuffer(sources[index], offset, destinations[index], offset, 512);
                    }
                    list.endMarker();
                    lists[index] = &list;
                });
            }
            for (std::thread& recorder : recorders) {
                recorder.join();
            }
            const Expected<void, Error> submitted = device->submit(lists);
            if (!submitted) {
                FAIL(submitted.error().toString());
            }
            REQUIRE(device->endFrame().hasValue());
        }
        stop.store(true, std::memory_order_release);
        for (std::thread& churner : churners) {
            churner.join();
        }
        CHECK(churned.load() > 0);
        REQUIRE(device->waitIdle().hasValue());
        CHECK(device->pendingDestructionCount() == 0);
    }
}

TEST_SUITE("rhi_d3d12.swapchains") {
    TEST_CASE("a swapchain on a hidden window presents, resizes, skips a zero size and is destroyed") {
        const HiddenWindow window;
        if (window.hwnd == nullptr) {
            MESSAGE("skipped: no desktop to create a window on");
            return;
        }
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        Expected<SwapchainHandle, Error> swapchain = device->createSwapchain(
            {.window = window.handle, .width = 256, .height = 256, .debugName = "Hidden window"});
        if (!swapchain) {
            FAIL(swapchain.error().toString());
            return;
        }

        TextureHandle firstImage;
        for (int frame = 0; frame < 4; ++frame) {
            const std::optional<Frame> presented = presentFrame(*device, *swapchain);
            if (!presented) {
                FAIL("no frame was presented");
                return;
            }
            CHECK(presented->width == 256);
            CHECK(presented->height == 256);
            CHECK(presented->backbufferFormat == Format::B8G8R8A8Srgb);
            if (frame == 0) {
                firstImage = presented->backbuffer;
            }
        }

        device->resizeSwapchain(*swapchain, 128, 64);
        const std::optional<Frame> resized = presentFrame(*device, *swapchain);
        if (!resized) {
            FAIL("no frame was presented");
            return;
        }
        CHECK(resized->width == 128);
        CHECK(resized->height == 64);
        CHECK_FALSE(device->isValid(firstImage));

        device->resizeSwapchain(*swapchain, 0, 0); // minimized
        CHECK_FALSE(presentFrame(*device, *swapchain).has_value());

        device->resizeSwapchain(*swapchain, 64, 64);
        const std::optional<Frame> restored = presentFrame(*device, *swapchain);
        if (!restored) {
            FAIL("no frame was presented");
            return;
        }
        CHECK(restored->width == 64);

        device->destroy(*swapchain);
        CHECK_FALSE(device->isValid(*swapchain));
        CHECK_FALSE(device->isValid(restored->backbuffer));
    }
}

#ifdef ITH_TEST_HAS_SHADER_COMPILER

namespace {

// Writes value + i into word i of the buffer whose bindless write index is in the constants.
constexpr std::string_view kFillSource = R"(
struct Constants {
    uint output;
    uint value;
};
ConstantBuffer<Constants> constants : register(b0, space0);

[numthreads(64, 1, 1)]
void fill(uint3 id : SV_DispatchThreadID) {
    RWByteAddressBuffer output = ResourceDescriptorHeap[constants.output];
    output.Store(id.x * 4, constants.value + id.x);
}
)";

// A triangle that covers the target, in the color from the constants.
constexpr std::string_view kDrawSource = R"(
struct Constants {
    float4 color;
};
ConstantBuffer<Constants> constants : register(b0, space0);

float4 cover(uint vertexId : SV_VertexID) : SV_Position {
    const float2 corner = float2(float((vertexId << 1) & 2), float(vertexId & 2));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

float4 paint() : SV_Target {
    return constants.color;
}
)";

std::vector<std::byte> compile(std::string_view source, std::string_view entryPoint, ith::ShaderStage stage) {
    ith::Expected<ith::ShaderCompiler> compiler = ith::ShaderCompiler::create();
    if (!compiler) {
        FAIL(compiler.error().message);
        return {};
    }
    ith::Expected<std::vector<std::byte>> code = compiler->compile({.name = "Test.slang",
                                                                    .source = source,
                                                                    .entryPoint = entryPoint,
                                                                    .stage = stage,
                                                                    .target = ith::ShaderTarget::Dxil});
    if (!code) {
        FAIL(code.error().message);
        return {};
    }
    return std::move(*code);
}

std::uint32_t readWord(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t word = 0;
    std::memcpy(&word, bytes.data() + offset, sizeof(word));
    return word;
}

struct FillConstants {
    std::uint32_t output = 0;
    std::uint32_t value = 0;
};

} // namespace

TEST_SUITE("rhi_d3d12.pipelines") {
    TEST_CASE("a compute shader writes a bindless buffer from root constants, directly and indirectly") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const std::vector<std::byte> code = compile(kFillSource, "fill", ith::ShaderStage::Compute);
        if (code.empty()) {
            return; // the failure is recorded
        }
        Expected<PipelineHandle, Error> pipeline = device->createComputePipeline(
            {.shader = {.stage = ShaderStage::Compute, .code = code, .entryPoint = "fill", .constantsSize = 8},
             .debugName = "Fill"});
        if (!pipeline) {
            FAIL(pipeline.error().toString());
            return;
        }
        constexpr std::uint64_t kWords = 128;
        const BufferHandle output =
            makeBuffer(*device, BufferUsage::ShaderWrite | BufferUsage::CopySrc, MemoryType::GpuOnly, kWords * 4);
        const BufferHandle arguments =
            makeBuffer(*device, BufferUsage::Indirect | BufferUsage::CopySrc, MemoryType::Upload, 64);
        const BufferHandle readback = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, kWords * 4);
        const DispatchIndirectArguments oneGroup{.groupCountX = 1, .groupCountY = 1, .groupCountZ = 1};
        std::memcpy(device->mappedMemory(arguments).data(), &oneGroup, sizeof(oneGroup));

        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Fill");
        list.setPipeline(*pipeline);
        list.pushConstants(FillConstants{.output = device->shaderWriteIndex(output), .value = 1000});
        list.dispatch(2); // words 0 to 127
        list.barrier(BufferBarrier{.buffer = output,
                                   .stagesBefore = PipelineStage::ComputeShader,
                                   .accessBefore = Access::ShaderWrite,
                                   .stagesAfter = PipelineStage::ComputeShader,
                                   .accessAfter = Access::ShaderWrite});
        list.pushConstants(FillConstants{.output = device->shaderWriteIndex(output), .value = 5000});
        list.dispatchIndirect(arguments, 0); // words 0 to 63 again
        list.barrier(BufferBarrier{.buffer = output,
                                   .stagesBefore = PipelineStage::ComputeShader,
                                   .accessBefore = Access::ShaderWrite,
                                   .stagesAfter = PipelineStage::Copy,
                                   .accessAfter = Access::CopyRead});
        list.copyBuffer(output, 0, readback, 0, kWords * 4);
        finish(*device, list);

        const std::span<const std::byte> words = device->mappedMemory(readback);
        CHECK(readWord(words, 0) == 5000);
        CHECK(readWord(words, 63 * 4) == 5063);
        CHECK(readWord(words, 64 * 4) == 1064);
        CHECK(readWord(words, 127 * 4) == 1127);
    }

    TEST_CASE("draws, indexed draws and indirect indexed draws paint the target with root-constant colors") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const std::vector<std::byte> vertex = compile(kDrawSource, "cover", ith::ShaderStage::Vertex);
        const std::vector<std::byte> pixel = compile(kDrawSource, "paint", ith::ShaderStage::Pixel);
        if (vertex.empty() || pixel.empty()) {
            return;
        }
        const std::array shaders{
            ShaderDesc{.stage = ShaderStage::Vertex, .code = vertex, .entryPoint = "cover"},
            ShaderDesc{.stage = ShaderStage::Pixel, .code = pixel, .entryPoint = "paint", .constantsSize = 16}};
        const std::array targets{ColorTarget{.format = Format::R8G8B8A8Unorm}};
        Expected<PipelineHandle, Error> pipeline = device->createGraphicsPipeline(
            {.shaders = shaders, .colorTargets = targets, .cullMode = CullMode::None, .debugName = "Cover"});
        if (!pipeline) {
            FAIL(pipeline.error().toString());
            return;
        }

        const BufferHandle indices = makeBuffer(*device, BufferUsage::Index, MemoryType::Upload, 12);
        const std::array<std::uint32_t, 3> triangle{0, 1, 2};
        std::memcpy(device->mappedMemory(indices).data(), triangle.data(), sizeof(triangle));
        const BufferHandle indirect = makeBuffer(*device, BufferUsage::Indirect, MemoryType::Upload, 256);
        const DrawIndexedIndirectArguments draw{.indexCount = 3, .instanceCount = 1};
        const std::uint32_t drawCount = 1;
        std::memcpy(device->mappedMemory(indirect).data(), &draw, sizeof(draw));
        std::memcpy(device->mappedMemory(indirect).data() + 128, &drawCount, sizeof(drawCount));

        const std::array<std::array<float, 4>, 3> colors{
            {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}};
        const std::array<std::array<int, 4>, 3> expected{{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}}};
        for (std::size_t kind = 0; kind < colors.size(); ++kind) {
            CAPTURE(kind);
            const TextureHandle target = makeTexture(*device, targetDesc());
            const BufferHandle readback = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, 1024);
            if (!beginFrame(*device)) {
                return;
            }
            CommandList& list = device->openCommandList("Draw");
            list.barrier(toRenderTarget(target));
            const std::array attachments{ColorAttachment{.texture = target}};
            list.beginRendering({.colors = attachments});
            list.setPipeline(*pipeline);
            list.pushConstants(colors[kind]);
            if (kind == 0) {
                list.draw(3);
            } else {
                list.setIndexBuffer(indices, 0, IndexFormat::Uint32);
                if (kind == 1) {
                    list.drawIndexed(3);
                } else {
                    list.drawIndexedIndirectCount(indirect, 0, indirect, 128, 4);
                }
            }
            list.endRendering();
            readBack(list, target, readback);
            finish(*device, list);
            const std::span<const std::byte> pixels = device->mappedMemory(readback);
            checkTexel(pixels, 0, 0, expected[kind]);
            checkTexel(pixels, 3, 3, expected[kind]);
            device->destroy(target);
            device->destroy(readback);
        }
    }

    TEST_CASE("a shader whose constants are not at b0 is refused") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        constexpr std::string_view kSource = R"(
ConstantBuffer<uint4> elsewhere : register(b3, space1);
[numthreads(1, 1, 1)]
void misplaced(uint3 id : SV_DispatchThreadID) {
    RWByteAddressBuffer output = ResourceDescriptorHeap[elsewhere.x];
    output.Store(0, id.x);
}
)";
        const std::vector<std::byte> code = compile(kSource, "misplaced", ith::ShaderStage::Compute);
        if (code.empty()) {
            return; // the failure is recorded
        }
        // The debug layer reports the mismatch as an error, which the backend asserts.
        const ith::test::AssertCapture capture;
        const Expected<PipelineHandle, Error> pipeline = device->createComputePipeline(
            {.shader = {.stage = ShaderStage::Compute, .code = code, .entryPoint = "misplaced"},
             .debugName = "Misplaced"});
        REQUIRE_FALSE(pipeline.hasValue());
        CHECK(pipeline.error().code == ErrorCode::InvalidShader);
    }
}

#endif
