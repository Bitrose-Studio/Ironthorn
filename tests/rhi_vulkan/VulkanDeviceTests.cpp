// The Vulkan backend on a GPU: the fastest hardware GPU, or a software implementation such as
// lavapipe when there is none (ITH_RHI_TEST_ADAPTER=software or =hardware picks one). The
// validation layer runs with synchronization validation outside Shipping when it is installed
// (ITH_RHI_TEST_VALIDATION=off turns it off), and asserts on every error it reports.
//
// Without a Vulkan loader or driver, as on hosted CI runners, every test reports that it is
// skipped and passes; ITH_RHI_TEST_REQUIRE_VULKAN=1 makes that a failure instead, for machines
// that must run them.
//
// The shaders are compiled from Slang at run time when the shader compiler is built
// (IRONTHORN_BUILD_TOOLS); without it, the pipeline and draw tests are skipped.

#include "ith/rhi_vulkan/VulkanDevice.h"

// The backend's SPIR-V check and GPU timing choice are internal; their tests need no GPU.
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"
#include "ith/rhi_vulkan/SpirvCheck.h"
#include "ith/rhi_vulkan/VulkanGpuTiming.h"

#ifdef ITH_TEST_HAS_SHADER_COMPILER
#include "ith/shader_compiler/ShaderCompiler.h"
#endif

#include "AssertCapture.h"

#include <doctest/doctest.h>

// The swapchain test makes its window with Win32 directly; elsewhere it is skipped.
#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before any thread starts
    return value != nullptr ? value : "";
#endif
}

// Shipping builds never load the validation layer.
#ifdef ITH_SHIPPING
constexpr bool kShipping = true;
#else
constexpr bool kShipping = false;
#endif

bool testValidation() {
    return environment("ITH_RHI_TEST_VALIDATION") != "off";
}

// The adapter the suite runs on, or std::nullopt (after reporting why) when the machine has no
// Vulkan device; chosen once.
std::optional<AdapterPreference> testAdapter() {
    static const std::optional<AdapterPreference> adapter = [] {
        const std::string chosen = environment("ITH_RHI_TEST_ADAPTER");
        std::vector<AdapterPreference> candidates;
        if (chosen != "software") {
            candidates.push_back(AdapterPreference::HighPerformance);
        }
        if (chosen != "hardware") {
            candidates.push_back(AdapterPreference::Software);
        }
        std::string reasons;
        for (const AdapterPreference candidate : candidates) {
            const Expected<Device, Error> device =
                createVulkanDevice({.adapter = candidate, .validation = testValidation()});
            if (device) {
                return std::optional<AdapterPreference>(candidate);
            }
            reasons += " " + device.error().toString();
        }
        std::fprintf(stderr, "rhi_vulkan: no Vulkan device:%s\n", reasons.c_str());
        return std::optional<AdapterPreference>();
    }();
    return adapter;
}

// doctest runs without exceptions here, so a failed REQUIRE or FAIL doesn't stop the test case
// (TestMain.cpp). The helpers below therefore never read the value of a failed ith::Expected:
// they record the failure and return a null handle, or a value the test case checks before it
// goes on.

// A device on the test adapter, or std::nullopt when the machine has none (the test then reports
// itself skipped, or fails when ITH_RHI_TEST_REQUIRE_VULKAN=1) or creation failed (recorded).
std::optional<Device> makeDevice() {
    const std::optional<AdapterPreference> adapter = testAdapter();
    if (!adapter) {
        if (environment("ITH_RHI_TEST_REQUIRE_VULKAN") == "1") {
            FAIL("no Vulkan device, and ITH_RHI_TEST_REQUIRE_VULKAN=1 requires one (see stderr)");
        }
        MESSAGE("skipped: no Vulkan loader, driver or device (see stderr)");
        return std::nullopt;
    }
    Expected<Device, Error> device = createVulkanDevice({.adapter = *adapter, .validation = testValidation()});
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

// Orders copies against the copies of earlier lists (synchronization validation checks it).
void afterEarlierCopies(CommandList& list) {
    list.barrier(GlobalBarrier{.stagesBefore = PipelineStage::Copy,
                               .accessBefore = Access::CopyWrite,
                               .stagesAfter = PipelineStage::Copy,
                               .accessAfter = Access::CopyRead | Access::CopyWrite});
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

TEST_SUITE("rhi_vulkan.device") {
    TEST_CASE("a device reports Vulkan, SPIR-V and its adapter") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const Capabilities& capabilities = device->capabilities();
        MESSAGE("adapter: " << capabilities.adapter.name);
        CHECK(capabilities.backend == Backend::Vulkan);
        CHECK(capabilities.shaderFormat == ShaderFormat::Spirv);
        CHECK_FALSE(capabilities.adapter.name.empty());
        CHECK(capabilities.adapter.software == (testAdapter() == AdapterPreference::Software));
        CHECK(capabilities.limits.resourceDescriptors == 262'144);
        CHECK(capabilities.limits.samplerDescriptors == 2'048);
        CHECK(capabilities.limits.maxTextureDimension2D >= 4'096);
        CHECK_FALSE(device->isLost());
    }

    TEST_CASE("createDevice creates a Vulkan device when asked for one") {
        const std::optional<AdapterPreference> adapter = testAdapter();
        if (!adapter) {
            MESSAGE("skipped: no Vulkan device");
            return;
        }
        Expected<Device, Error> device =
            createDevice({.adapter = *adapter, .validation = testValidation()}, Backend::Vulkan);
        if (!device) {
            FAIL(device.error().toString());
            return;
        }
        CHECK(device->capabilities().backend == Backend::Vulkan);
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
                                  .usage = TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::RenderTarget,
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

    TEST_CASE("bytes that are not SPIR-V are refused as invalid shaders") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const std::array<std::byte, 64> notSpirv{};
        const Expected<PipelineHandle, Error> pipeline = device->createComputePipeline(
            {.shader = {.stage = ShaderStage::Compute, .code = notSpirv}, .debugName = "Not SPIR-V"});
        REQUIRE_FALSE(pipeline.hasValue());
        CHECK(pipeline.error().code == ErrorCode::InvalidShader);
        CHECK(pipeline.error().message.find("Not SPIR-V") != std::string::npos);
    }
}

TEST_SUITE("rhi_vulkan.frames") {
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

    TEST_CASE("many frames with lists recorded on the pooled command pools run back to back") {
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
            afterEarlierCopies(first);
            first.beginMarker("Copy");
            first.copyBuffer(source, 0, destination, 0, 512);
            first.endMarker();
            afterEarlierCopies(second);
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

namespace {

#ifdef ITH_ENABLE_PROFILING
constexpr bool kProfiling = true;
#else
constexpr bool kProfiling = false;
#endif

// Whether GPU zones are active without a viewer (IRONTHORN_PROFILING_ON_DEMAND off), so that they
// write timestamps in these tests.
#if defined(ITH_ENABLE_PROFILING) && !defined(TRACY_ON_DEMAND)
constexpr bool kZonesActive = true;
#else
constexpr bool kZonesActive = false;
#endif

// Frames with GPU zones: nested, around and inside rendering, and one in a list that is discarded
// and never runs; the last frame reads back a cleared target, which the zones must not change.
// The zones and the collection at each endFrame() must leave the commands and the validation
// layer alone. They write timestamps only where zones are active (kZonesActive, or a viewer).
void recordZoneFrames(Device& device) {
    const TextureHandle target = makeTexture(device, targetDesc());
    const BufferHandle readback = makeBuffer(device, BufferUsage::CopyDst, MemoryType::Readback, 1024);
    constexpr int kFrames = 4;
    for (int frame = 0; frame < kFrames; ++frame) {
        if (!beginFrame(device)) {
            return;
        }
        // In the last frame: it turns the device's GPU zones off where they are active.
        if (frame + 1 == kFrames) {
            CommandList& discarded = device.openCommandList("Discarded");
            discarded.beginGpuZone("Never runs");
            discarded.endGpuZone();
            device.discard(discarded);
        }

        CommandList& list = device.openCommandList("Zones");
        list.beginGpuZone("Frame");
        list.beginGpuZone("Clear");
        TextureBarrier toTarget = toRenderTarget(target);
        if (frame > 0) {
            // After the previous frame's clear.
            toTarget.stagesBefore = PipelineStage::ColorOutput;
            toTarget.accessBefore = Access::RenderTargetWrite;
        }
        list.barrier(toTarget);
        const std::array colors{ColorAttachment{.texture = target, .clearColor = {0.25f, 0.5f, 0.75f, 1.0f}}};
        list.beginRendering({.colors = colors});
        list.beginGpuZone("Inside rendering");
        list.endGpuZone();
        list.endRendering();
        list.endGpuZone();
        if (frame + 1 == kFrames) {
            list.beginGpuZone("Read back");
            readBack(list, target, readback);
            list.endGpuZone();
        }
        list.endGpuZone();
        REQUIRE(device.submit(list).hasValue());
        REQUIRE(device.endFrame().hasValue());
    }
    REQUIRE(device.waitIdle().hasValue());
    const std::span<const std::byte> pixels = device.mappedMemory(readback);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            checkTexel(pixels, x, y, {64, 128, 191, 255});
        }
    }
}

// 70 frames of 512 zones: 71,680 queries where zones are active, more than Tracy's pool holds.
void recordManyZones(Device& device) {
    constexpr int kFrames = 70;
    constexpr int kZonesPerFrame = 512;
    for (int frame = 0; frame < kFrames; ++frame) {
        if (!beginFrame(device)) {
            return;
        }
        CommandList& list = device.openCommandList("Many zones");
        for (int zone = 0; zone < kZonesPerFrame; ++zone) {
            list.beginGpuZone("Zone");
            list.endGpuZone();
        }
        REQUIRE(device.submit(list).hasValue());
        REQUIRE(device.endFrame().hasValue());
    }
    REQUIRE(device.waitIdle().hasValue());
}

const char* timingName(VulkanGpuTiming timing) {
    switch (timing) {
        case VulkanGpuTiming::Off:
            return "off";
        case VulkanGpuTiming::Calibrated:
            return "calibrated";
        case VulkanGpuTiming::Uncalibrated:
            return "uncalibrated";
    }
    return "?";
}

} // namespace

TEST_SUITE("rhi_vulkan.profiling") {
    TEST_CASE("GPU zones are calibrated only with the extension, a host clock and host query reset") {
        using ith::rhi::vulkan::chooseGpuTiming;
        using ith::rhi::vulkan::GpuTimingSupport;
        constexpr GpuTimingSupport kEverything{
            .timestamps = true, .hostQueryReset = true, .calibratedTimestamps = true, .hostTimeDomain = true};
        static_assert(chooseGpuTiming(kEverything) == VulkanGpuTiming::Calibrated);

        GpuTimingSupport support = kEverything;
        support.hostTimeDomain = false; // the extension without the host's clock
        CHECK(chooseGpuTiming(support) == VulkanGpuTiming::Uncalibrated);
        support = kEverything;
        support.calibratedTimestamps = false; // no extension, or only VK_KHR_calibrated_timestamps
        support.hostTimeDomain = false;
        CHECK(chooseGpuTiming(support) == VulkanGpuTiming::Uncalibrated);
        support = kEverything;
        support.hostQueryReset = false; // the calibrated context resets its queries from the host once
        CHECK(chooseGpuTiming(support) == VulkanGpuTiming::Uncalibrated);
        support = GpuTimingSupport{.timestamps = true};
        CHECK(chooseGpuTiming(support) == VulkanGpuTiming::Uncalibrated);
        support = kEverything;
        support.timestamps = false;
        CHECK(chooseGpuTiming(support) == VulkanGpuTiming::Off);
        CHECK(chooseGpuTiming({}) == VulkanGpuTiming::Off);
    }

    TEST_CASE("GPU zones over frames leave the commands' results and the validation layer alone") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const VulkanGpuTiming timing = vulkanGpuTimingForTesting(*device);
        MESSAGE("GPU zones on " << device->capabilities().adapter.name << ": " << std::string(timingName(timing)));
        if (!kProfiling) {
            CHECK(timing == VulkanGpuTiming::Off);
        }
        recordZoneFrames(*device);
    }

    TEST_CASE("a device whose calibrated timestamps are avoided times GPU zones uncalibrated") {
        avoidCalibratedVulkanGpuTimingForTesting(true);
        std::optional<Device> device = makeDevice();
        avoidCalibratedVulkanGpuTimingForTesting(false);
        if (!device) {
            return;
        }
        // Not calibrated; uncalibrated on every device the suite runs on (desktop drivers,
        // lavapipe), which writes one timestamp on the queue at creation.
        const VulkanGpuTiming timing = vulkanGpuTimingForTesting(*device);
        MESSAGE("GPU zones with calibration avoided: " << std::string(timingName(timing)));
        CHECK(timing != VulkanGpuTiming::Calibrated);
        if (!kProfiling) {
            CHECK(timing == VulkanGpuTiming::Off);
        }
        recordZoneFrames(*device);
    }

    // A list with zones that never runs leaves queries unwritten. Tracy collects them in order and
    // would wait for them for good, and once its 65,536 queries had wrapped around, zones would
    // write queries that were never reset (VUID-vkCmdWriteTimestamp-None-00830), which validation
    // reports. The backend turns GPU zones off for the device instead (said once on stderr). Where
    // zones are active, each case drops a list with zones and then records 71,680 queries' worth;
    // elsewhere it checks that nothing changes.
    TEST_CASE("zones of a discarded list that isn't the first one turn the device's GPU zones off") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        MESSAGE("zones active without a viewer: " << std::string(kZonesActive ? "yes" : "no"));
        if (!beginFrame(*device)) {
            return;
        }
        CommandList& first = device->openCommandList("Runs");
        CommandList& second = device->openCommandList("Never runs"); // the second list of its pool
        first.beginGpuZone("Runs");
        first.endGpuZone();
        second.beginGpuZone("Dropped");
        second.endGpuZone();
        CHECK_FALSE(vulkanGpuZonesDisabledForTesting(*device));
        device->discard(second);
        CHECK(vulkanGpuZonesDisabledForTesting(*device) == kZonesActive);
        REQUIRE(device->submit(first).hasValue());
        REQUIRE(device->endFrame().hasValue());
        recordManyZones(*device);
        CHECK(vulkanGpuZonesDisabledForTesting(*device) == kZonesActive);
    }

    TEST_CASE("zones of a list whose submission failed turn the device's GPU zones off") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Submission fails");
        list.beginGpuZone("Never runs");
        list.endGpuZone();
        failNextVulkanSubmitForTesting(*device, true);
        const Expected<void, Error> submitted = device->submit(list);
        REQUIRE_FALSE(submitted.hasValue());
        CHECK(submitted.error().code == ErrorCode::OutOfMemory);
        CHECK_FALSE(device->isLost());
        CHECK(vulkanGpuZonesDisabledForTesting(*device) == kZonesActive);
        REQUIRE(device->endFrame().hasValue());
        recordManyZones(*device);
    }

    // A list still open at endFrame() (asserted) may still be recording on another thread, so the
    // backend turns GPU zones off without looking at what it recorded, viewer or not.
    TEST_CASE("a list endFrame() finds still open turns the device's GPU zones off") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const bool profiled = vulkanGpuTimingForTesting(*device) != VulkanGpuTiming::Off;
        if (!beginFrame(*device)) {
            return;
        }
        static_cast<void>(device->openCommandList("Left open"));
        {
            const ith::test::AssertCapture capture;
            REQUIRE(device->endFrame().hasValue());
        }
        CHECK(vulkanGpuZonesDisabledForTesting(*device) == profiled);
        recordManyZones(*device);
    }
}

TEST_SUITE("rhi_vulkan.commands") {
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
}

namespace {

// A SPIR-V module the shader compiler wrote, from tests/rhi_vulkan/fuzz_seeds (the tests run in
// the repository's root).
std::vector<std::byte> spirvSeed(const char* name) {
    std::ifstream file(std::string("tests/rhi_vulkan/fuzz_seeds/") + name, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> code(bytes.size());
    std::memcpy(code.data(), bytes.data(), bytes.size());
    return code;
}

constexpr ith::rhi::vulkan::BindlessCounts kCounts{.resources = 262'144, .samplers = 2'048};

} // namespace

// The binding check reads untrusted bytes without a GPU (its fuzz target runs it further).
TEST_SUITE("rhi_vulkan.spirv") {
    TEST_CASE("a module of the bindless set's four bindings is accepted") {
        const std::vector<std::byte> code = spirvSeed("bindless.spv");
        REQUIRE(ith::rhi::vulkan::isSpirv(code));
        const std::optional<std::string> problem = ith::rhi::vulkan::bindingProblem(code, kCounts);
        CHECK_FALSE(problem.has_value());
        if (problem) {
            MESSAGE(*problem);
        }
    }

    TEST_CASE("a uniform buffer and another descriptor set are refused") {
        const std::vector<std::byte> code = spirvSeed("outside.spv");
        REQUIRE(ith::rhi::vulkan::isSpirv(code));
        CHECK(ith::rhi::vulkan::bindingProblem(code, kCounts).has_value());
    }

    TEST_CASE("a module cut short or claiming more ids than it can hold is refused") {
        std::vector<std::byte> code = spirvSeed("bindless.spv");
        REQUIRE(code.size() > 64);
        std::vector<std::byte> cut(code.begin(), code.begin() + 64);
        CHECK(ith::rhi::vulkan::bindingProblem(cut, kCounts).has_value());
        // An id bound of about four billion in a 24-byte module: refused without allocating.
        std::vector<std::byte> header(code.begin(), code.begin() + 24);
        const std::uint32_t huge = 0xFFFF'FFF0u;
        std::memcpy(header.data() + 12, &huge, sizeof(huge));
        REQUIRE(ith::rhi::vulkan::isSpirv(header));
        CHECK(ith::rhi::vulkan::bindingProblem(header, kCounts).has_value());
        CHECK_FALSE(ith::rhi::vulkan::isSpirv(std::span<const std::byte>(code.data(), 3)));
    }
}

TEST_SUITE("rhi_vulkan.validation") {
    TEST_CASE("synchronization validation reports a missing barrier as an error") {
        if (!testValidation() || kShipping) {
            MESSAGE("skipped: validation is off (Shipping, or ITH_RHI_TEST_VALIDATION=off)");
            return;
        }
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const BufferHandle source = makeBuffer(*device, BufferUsage::CopySrc, MemoryType::Upload, 256);
        const BufferHandle destination = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::GpuOnly, 256);
        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Missing barrier");
        {
            // The second copy writes what the first wrote, with no barrier between them.
            const ith::test::AssertCapture capture;
            list.copyBuffer(source, 0, destination, 0, 256);
            list.copyBuffer(source, 0, destination, 0, 256);
            if (capture.count() == 0) {
                // The layer is not installed; machines that must have it set ITH_RHI_TEST_REQUIRE_VULKAN.
                MESSAGE("the validation layer reported nothing: it is probably not installed");
                CHECK(environment("ITH_RHI_TEST_REQUIRE_VULKAN") != "1");
            } else {
                CHECK(capture.lastMessage().find("validation layer reported an error") != std::string::npos);
            }
        }
        device->discard(list);
        REQUIRE(device->endFrame().hasValue());
    }
}

TEST_SUITE("rhi_vulkan.validation") {
    // Barrier.h: a barrier out of Undefined still follows the texture's earlier writes, and must
    // name them. Without the access, the layout change races the earlier depth write
    // (SYNC-HAZARD-WRITE-AFTER-WRITE on stderr).
    TEST_CASE("a depth buffer discarded after a depth write must name that write as the access before") {
        if (!testValidation() || kShipping) {
            MESSAGE("skipped: validation is off (Shipping, or ITH_RHI_TEST_VALIDATION=off)");
            return;
        }
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const TextureHandle depth = makeTexture(*device, {.width = 4,
                                                          .height = 4,
                                                          .format = Format::D32Float,
                                                          .usage = TextureUsage::DepthStencil,
                                                          .debugName = "Reused depth"});
        for (const bool namesWrite : {true, false}) {
            CAPTURE(namesWrite);
            if (!beginFrame(*device)) {
                return;
            }
            CommandList& list = device->openCommandList("Depth reuse");
            list.barrier(TextureBarrier{.texture = depth,
                                        .stagesBefore = PipelineStage::DepthStencil,
                                        .accessBefore = Access::DepthStencilWrite,
                                        .stagesAfter = PipelineStage::DepthStencil,
                                        .accessAfter = Access::DepthStencilWrite,
                                        .layoutBefore = TextureLayout::Undefined,
                                        .layoutAfter = TextureLayout::DepthStencilWrite});
            list.beginRendering({.depth = {.texture = depth}}); // the clear writes depth
            list.endRendering();
            {
                const ith::test::AssertCapture capture;
                list.barrier(TextureBarrier{.texture = depth,
                                            .stagesBefore = PipelineStage::DepthStencil,
                                            .accessBefore = namesWrite ? Access::DepthStencilWrite : Access::None,
                                            .stagesAfter = PipelineStage::DepthStencil,
                                            .accessAfter = Access::DepthStencilWrite,
                                            .layoutBefore = TextureLayout::Undefined,
                                            .layoutAfter = TextureLayout::DepthStencilWrite});
                list.beginRendering({.depth = {.texture = depth}});
                list.endRendering();
                if (namesWrite) {
                    CHECK(capture.count() == 0);
                } else if (capture.count() == 0) {
                    // The layer is not installed; machines that must have it set ITH_RHI_TEST_REQUIRE_VULKAN.
                    MESSAGE("the validation layer reported nothing: it is probably not installed");
                    CHECK(environment("ITH_RHI_TEST_REQUIRE_VULKAN") != "1");
                } else {
                    CHECK(capture.lastMessage().find("validation layer reported an error") != std::string::npos);
                }
            }
            device->discard(list);
            REQUIRE(device->endFrame().hasValue());
        }
        REQUIRE(device->waitIdle().hasValue());
        device->destroy(depth);
    }
}

TEST_SUITE("rhi_vulkan.device_loss") {
    TEST_CASE("a lost device reports device loss from submit and every later call") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        const BufferHandle buffer = makeBuffer(*device, BufferUsage::CopyDst, MemoryType::Readback, 256);
        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Lost");
        failNextVulkanSubmitForTesting(*device);
        const Expected<void, Error> submitted = device->submit(list);
        REQUIRE_FALSE(submitted.hasValue());
        CHECK(submitted.error().code == ErrorCode::DeviceLost);
        MESSAGE(submitted.error().toString());
        CHECK(submitted.error().message.find("Vulkan device was lost") != std::string::npos);
        static_cast<void>(device->endFrame());
        CHECK(device->isLost());

        const Expected<BufferHandle, Error> another = device->createBuffer(
            {.size = 4, .usage = BufferUsage::CopyDst, .memory = MemoryType::Readback, .debugName = "Late"});
        REQUIRE_FALSE(another.hasValue());
        CHECK(another.error().code == ErrorCode::DeviceLost);
        const Expected<void, Error> idle = device->waitIdle();
        REQUIRE_FALSE(idle.hasValue());
        CHECK(idle.error().code == ErrorCode::DeviceLost);
        const Expected<std::optional<Frame>, Error> frame = device->beginFrame();
        REQUIRE_FALSE(frame.hasValue());
        CHECK(frame.error().code == ErrorCode::DeviceLost);
        device->destroy(buffer);
    }

    TEST_CASE("a lost device at the end of a frame reports device loss and nothing waits forever") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        if (!beginFrame(*device)) {
            return;
        }
        CommandList& list = device->openCommandList("Before the loss");
        REQUIRE(device->submit(list).hasValue());
        failNextVulkanSubmitForTesting(*device);
        const Expected<void, Error> ended = device->endFrame();
        REQUIRE_FALSE(ended.hasValue());
        CHECK(ended.error().code == ErrorCode::DeviceLost);
        CHECK(device->isLost());
        CHECK(device->waitIdle().error().code == ErrorCode::DeviceLost);
    }
}

namespace {

// A render target first used without a clear, then cleared in a second rendering and read back.
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
    list.barrier(TextureBarrier{.texture = target,
                                .stagesBefore = PipelineStage::ColorOutput,
                                .accessBefore = Access::RenderTargetWrite,
                                .stagesAfter = PipelineStage::ColorOutput,
                                .accessAfter = Access::RenderTargetWrite,
                                .layoutBefore = TextureLayout::RenderTarget,
                                .layoutAfter = TextureLayout::RenderTarget});
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

} // namespace

TEST_SUITE("rhi_vulkan.textures") {
    TEST_CASE("a render target first used without a clear, then cleared, reads back") {
        std::optional<Device> device = makeDevice();
        if (!device) {
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

TEST_SUITE("rhi_vulkan.threads") {
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
                    afterEarlierCopies(list);
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

#ifdef _WIN32

namespace {

// A borderless window that is never shown, so that its client area is its size; its hwnd is null
// where there is no desktop.
struct HiddenWindow {
    HiddenWindow() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"IronthornVulkanTestWindow";
        RegisterClassExW(&windowClass); // fails harmlessly when already registered
        hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"Ironthorn test", WS_POPUP, 0, 0, 256, 256, nullptr,
                               nullptr, instance, nullptr);
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

    void resize(int width, int height) const {
        SetWindowPos(hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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

TEST_SUITE("rhi_vulkan.swapchains") {
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

        // The window's size decides the images' size on Win32: the window changes first, as it
        // does when the user resizes it.
        window.resize(128, 64);
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

        window.resize(64, 64);
        device->resizeSwapchain(*swapchain, 64, 64);
        const std::optional<Frame> restored = presentFrame(*device, *swapchain);
        if (!restored) {
            FAIL("no frame was presented");
            return;
        }
        CHECK(restored->width == 64);

        REQUIRE(device->waitIdle().hasValue());
        device->destroy(*swapchain);
        CHECK_FALSE(device->isValid(*swapchain));
        CHECK_FALSE(device->isValid(restored->backbuffer));
    }
}

#endif

#ifdef ITH_TEST_HAS_SHADER_COMPILER

namespace {

// Writes value + i into word i of the buffer whose bindless write index is in the constants. The
// constants are a push-constant block at offset 0 (on Direct3D 12, the same source's constant
// buffer at b0 holds them), and buffers are the bindless set's binding 0.
constexpr std::string_view kFillSource = R"(
struct Constants {
    uint output;
    uint value;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;
[[vk::binding(0, 0)]] RWByteAddressBuffer buffers[];

[numthreads(64, 1, 1)]
void fill(uint3 id : SV_DispatchThreadID) {
    buffers[constants.output].Store(id.x * 4, constants.value + id.x);
}
)";

// A triangle that covers the target, in the color from the constants.
constexpr std::string_view kDrawSource = R"(
struct Constants {
    float4 color;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

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
                                                                    .target = ith::ShaderTarget::Spirv});
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

TEST_SUITE("rhi_vulkan.pipelines") {
    TEST_CASE("a compute shader writes a bindless buffer from push constants, directly and indirectly") {
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

    TEST_CASE("draws, indexed draws and indirect indexed draws paint the target with push-constant colors") {
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

    TEST_CASE("a shader that binds resources outside the bindless set is refused") {
        std::optional<Device> device = makeDevice();
        if (!device) {
            return;
        }
        // Constants in a uniform buffer instead of push constants, and buffers in set 1.
        constexpr std::string_view kSource = R"(
ConstantBuffer<uint4> elsewhere;
[[vk::binding(0, 1)]] RWByteAddressBuffer buffers[];
[numthreads(1, 1, 1)]
void misplaced(uint3 id : SV_DispatchThreadID) {
    buffers[elsewhere.x].Store(0, id.x);
}
)";
        const std::vector<std::byte> code = compile(kSource, "misplaced", ith::ShaderStage::Compute);
        if (code.empty()) {
            return; // the failure is recorded
        }
        const Expected<PipelineHandle, Error> pipeline = device->createComputePipeline(
            {.shader = {.stage = ShaderStage::Compute, .code = code, .entryPoint = "misplaced"},
             .debugName = "Misplaced"});
        REQUIRE_FALSE(pipeline.hasValue());
        CHECK(pipeline.error().code == ErrorCode::InvalidShader);
        MESSAGE(pipeline.error().toString());
    }
}

#endif
