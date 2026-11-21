#include "ith/rhi/Device.h"

#include "ith/rhi/DeviceState.h"
#include "ith/rhi/NullDevice.h"

#include "AssertCapture.h"
#include "RhiTestSupport.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through it
#include <string_view>
#include <utility>

using namespace ith::rhi;
using ith::Expected;
using ith::rhi::test::kShaderCode;
using ith::rhi::test::makeBuffer;
using ith::rhi::test::makeDevice;
using ith::rhi::test::makeTexture;

TEST_SUITE("rhi.device") {
    TEST_CASE("the null device reports what it is, with the default limits") {
        Expected<Device, Error> device = createNullDevice();
        REQUIRE(device.hasValue());
        const Capabilities& capabilities = device->capabilities();
        CHECK(capabilities.backend == Backend::Null);
        CHECK(capabilities.shaderFormat == ShaderFormat::None);
        CHECK(capabilities.adapter.software);
        CHECK_FALSE(capabilities.adapter.name.empty());
        CHECK(capabilities.limits.resourceDescriptors == 262'144);
        CHECK(capabilities.limits.samplerDescriptors == 2'048);
        CHECK_FALSE(capabilities.meshShaders);
        CHECK_FALSE(device->isLost());
        CHECK(capabilities.limits.maxTextureDimension2D == 16'384);
        CHECK(device->frameNumber() == 0);
    }

    TEST_CASE("a GPU without the minimum features is refused, naming each missing feature") {
        const std::array<std::string_view, 2> missing{"Shader Model 6.6", "enhanced barriers"};
        const Expected<Device, Error> device = createNullDevice({.missingFeatures = missing});
        REQUIRE_FALSE(device.hasValue());
        CHECK(device.error().code == ErrorCode::UnsupportedDevice);
        CHECK(device.error().message.find("Shader Model 6.6, enhanced barriers") != std::string::npos);
        CHECK(device.error().toString().ends_with("[rhi.unsupported_device]"));
    }

    TEST_CASE("optional features are reported as configured") {
        NullGpu gpu;
        Capabilities capabilities = ith::rhi::test::testCapabilities();
        capabilities.meshShaders = true;
        capabilities.rayTracing = true;
        const Device device = makeDevice(gpu, capabilities);
        CHECK(device.capabilities().meshShaders);
        CHECK(device.capabilities().rayTracing);
        CHECK_FALSE(device.capabilities().variableRateShading);
        CHECK(device.capabilities().backend == Backend::Null);
    }

    TEST_CASE("every error code has a stable dotted name") {
        CHECK(codeName(ErrorCode::DeviceLost) == "rhi.device_lost");
        CHECK(codeName(ErrorCode::BackendUnavailable) == "rhi.backend_unavailable");
        CHECK(codeName(ErrorCode::UnsupportedDevice) == "rhi.unsupported_device");
        CHECK(codeName(ErrorCode::OutOfMemory) == "rhi.out_of_memory");
        CHECK(codeName(ErrorCode::InvalidUsage) == "rhi.invalid_usage");
        CHECK(codeName(ErrorCode::InvalidShader) == "rhi.invalid_shader");
        CHECK(codeName(ErrorCode::OutOfHandles) == "rhi.out_of_handles");
        CHECK(codeName(ErrorCode::OutOfDescriptors) == "rhi.out_of_descriptors");
        CHECK(codeName(ErrorCode::SurfaceFailed) == "rhi.surface_failed");
        CHECK(codeName(ErrorCode::BackendFailure) == "rhi.backend_failure");
        CHECK(Error{.code = ErrorCode::DeviceLost, .message = "Gone"}.toString() == "Gone [rhi.device_lost]");
    }
}

TEST_SUITE("rhi.handles") {
    TEST_CASE("the default handle is null and never valid") {
        NullGpu gpu;
        const Device device = makeDevice(gpu);
        const BufferHandle null;
        CHECK(null.isNull());
        CHECK_FALSE(device.isValid(null));
        CHECK_FALSE(device.isValid(TextureHandle{}));
        CHECK_FALSE(device.isValid(SamplerHandle{}));
        CHECK_FALSE(device.isValid(PipelineHandle{}));
        CHECK_FALSE(device.isValid(SwapchainHandle{}));
    }

    TEST_CASE("a destroyed object's handle is stale at once, and destroying the null handle does nothing") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device);
        CHECK(device.isValid(buffer));
        CHECK_FALSE(buffer.isNull());
        device.destroy(buffer);
        CHECK_FALSE(device.isValid(buffer));
        CHECK(device.pendingDestructionCount() == 1);
        device.destroy(BufferHandle{});
        CHECK(device.pendingDestructionCount() == 1);
    }

    TEST_CASE("destroying a stale handle asserts and changes nothing") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device);
        device.destroy(buffer);
        const ith::test::AssertCapture capture;
        device.destroy(buffer);
        CHECK(device.pendingDestructionCount() == 1);
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 1);
#endif
    }

    TEST_CASE("a released slot is reused under a new generation, so old handles stay stale") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle first = makeBuffer(device);
        device.destroy(first);
        REQUIRE(device.waitIdle().hasValue());
        CHECK(device.pendingDestructionCount() == 0);
        const BufferHandle second = makeBuffer(device);
        CHECK(second.index() == first.index());
        CHECK(second.generation() != first.generation());
        CHECK(device.isValid(second));
        CHECK_FALSE(device.isValid(first));
    }

    TEST_CASE("handles of different objects differ and every object type has its own table") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle a = makeBuffer(device);
        const BufferHandle b = makeBuffer(device);
        const TextureHandle texture = makeTexture(device);
        CHECK(a != b);
        CHECK(texture.index() == a.index()); // separate tables, both start at slot 0
        CHECK(device.isValid(texture));
        CHECK(gpu.liveObjectCount() == 3);
    }

    TEST_CASE("the device releases objects the caller did not destroy") {
        NullGpu gpu;
        {
            Device device = makeDevice(gpu);
            makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload);
            makeTexture(device);
            const BufferHandle destroyed = makeBuffer(device);
            device.destroy(destroyed);
            REQUIRE(device.createSampler({.debugName = "Leaked sampler"}).hasValue());
            REQUIRE(device.createSwapchain({.width = 8, .height = 8, .debugName = "Leaked"}).hasValue());
            CHECK(gpu.liveObjectCount() == 5);
        }
        CHECK(gpu.liveObjectCount() == 0);
    }

    TEST_CASE("a moved device keeps its objects") {
        NullGpu gpu;
        Device first = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(first);
        Device second = std::move(first);
        CHECK(second.isValid(buffer));
    }
}

TEST_SUITE("rhi.descriptors") {
    TEST_CASE("each shader usage gets its own bindless index") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device, BufferUsage::ShaderRead | BufferUsage::ShaderWrite);
        const TextureHandle texture = makeTexture(device, TextureUsage::Sampled | TextureUsage::Storage);
        const std::array indices{device.shaderReadIndex(buffer), device.shaderWriteIndex(buffer),
                                 device.sampledIndex(texture), device.storageIndex(texture)};
        for (std::size_t i = 0; i < indices.size(); ++i) {
            CHECK(indices[i] != kInvalidDescriptorIndex);
            for (std::size_t j = i + 1; j < indices.size(); ++j) {
                CHECK(indices[i] != indices[j]);
            }
        }
        const Expected<SamplerHandle, Error> sampler = device.createSampler({.debugName = "Linear"});
        REQUIRE(sampler.hasValue());
        CHECK(device.samplerIndex(*sampler) == 0); // its own heap
    }

    TEST_CASE("asking for an index of a usage the object lacks asserts") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device, BufferUsage::ShaderRead);
        const ith::test::AssertCapture capture;
        CHECK(device.shaderWriteIndex(buffer) == kInvalidDescriptorIndex);
        device.destroy(buffer);
        CHECK(device.shaderReadIndex(buffer) == kInvalidDescriptorIndex);
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 2);
#endif
    }

    TEST_CASE("a descriptor index is reused only after deferred destruction released its object") {
        NullGpu gpu;
        gpu.setAutoComplete(false);
        Device device = makeDevice(gpu);
        const BufferHandle first = makeBuffer(device);
        const std::uint32_t firstIndex = device.shaderReadIndex(first);

        ith::rhi::test::emptyFrame(device); // the GPU has not finished this frame
        device.destroy(first);
        const BufferHandle second = makeBuffer(device);
        CHECK(device.shaderReadIndex(second) != firstIndex);

        REQUIRE(device.waitIdle().hasValue());
        const BufferHandle third = makeBuffer(device);
        CHECK(device.shaderReadIndex(third) == firstIndex);
    }

    TEST_CASE("running out of descriptors fails with OutOfDescriptors and leaks nothing") {
        NullGpu gpu;
        Capabilities capabilities = ith::rhi::test::testCapabilities();
        capabilities.limits.resourceDescriptors = 2;
        Device device = makeDevice(gpu, capabilities);
        const BufferHandle both = makeBuffer(device, BufferUsage::ShaderRead | BufferUsage::ShaderWrite);
        const Expected<BufferHandle, Error> more =
            device.createBuffer({.size = 4, .usage = BufferUsage::ShaderRead, .debugName = "One too many"});
        REQUIRE_FALSE(more.hasValue());
        CHECK(more.error().code == ErrorCode::OutOfDescriptors);
        CHECK(more.error().message.find("'One too many'") != std::string::npos);
        // A buffer without shader usage needs no descriptor and still fits.
        CHECK(device.createBuffer({.size = 4, .usage = BufferUsage::CopyDst, .debugName = "Copy"}).hasValue());
        device.destroy(both);
        REQUIRE(device.waitIdle().hasValue());
        CHECK(device.createBuffer({.size = 4, .usage = BufferUsage::ShaderWrite, .debugName = "Fits"}).hasValue());
    }

    TEST_CASE("the sampler heap has its own capacity") {
        NullGpu gpu;
        Capabilities capabilities = ith::rhi::test::testCapabilities();
        capabilities.limits.samplerDescriptors = 1;
        Device device = makeDevice(gpu, capabilities);
        REQUIRE(device.createSampler({.debugName = "First"}).hasValue());
        const Expected<SamplerHandle, Error> second = device.createSampler({.debugName = "Second"});
        REQUIRE_FALSE(second.hasValue());
        CHECK(second.error().code == ErrorCode::OutOfDescriptors);
    }
}

TEST_SUITE("rhi.resources") {
    TEST_CASE("Upload and Readback buffers are persistently mapped") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle upload = makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload, 64);
        const std::span<std::byte> bytes = device.mappedMemory(upload);
        REQUIRE(bytes.size() == 64);
        bytes[63] = std::byte{7};
        CHECK(device.mappedMemory(upload)[63] == std::byte{7});
        const BufferHandle readback = makeBuffer(device, BufferUsage::CopyDst, MemoryType::Readback, 16);
        CHECK(device.mappedMemory(readback).size() == 16);
    }

    TEST_CASE("a GpuOnly buffer has no mapped memory") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device);
        const ith::test::AssertCapture capture;
        CHECK(device.mappedMemory(buffer).empty());
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 1);
#endif
    }

    TEST_CASE("descriptions that break their rules fail with InvalidUsage and assert") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const ith::test::AssertCapture capture;
        int expectedAsserts = 0;
        const auto refused = [&](const auto& result) {
            ++expectedAsserts;
            return !result.hasValue() && result.error().code == ErrorCode::InvalidUsage;
        };
        CHECK(refused(device.createBuffer({.size = 4, .usage = BufferUsage::ShaderRead})));
        CHECK(refused(device.createBuffer({.size = 0, .usage = BufferUsage::ShaderRead, .debugName = "Empty"})));
        CHECK(refused(device.createBuffer({.size = 4, .debugName = "No usage"})));
        CHECK(refused(device.createBuffer({.size = 4,
                                           .usage = BufferUsage::ShaderWrite,
                                           .memory = MemoryType::Upload,
                                           .debugName = "Writable upload"})));
        CHECK(refused(device.createBuffer(
            {.size = 4, .usage = BufferUsage::ShaderRead, .memory = MemoryType::Readback, .debugName = "Readback"})));
        CHECK(refused(device.createTexture({.width = 0,
                                            .height = 4,
                                            .format = Format::R8Unorm,
                                            .usage = TextureUsage::Sampled,
                                            .debugName = "Zero"})));
        CHECK(refused(
            device.createTexture({.width = 4, .height = 4, .usage = TextureUsage::Sampled, .debugName = "No format"})));
        CHECK(refused(device.createTexture({.width = 4,
                                            .height = 4,
                                            .mipLevels = 4,
                                            .format = Format::R8Unorm,
                                            .usage = TextureUsage::Sampled,
                                            .debugName = "Too many mips"})));
        CHECK(refused(device.createTexture({.dimension = TextureDimension::TextureCube,
                                            .width = 4,
                                            .height = 4,
                                            .layers = 4,
                                            .format = Format::R8Unorm,
                                            .usage = TextureUsage::Sampled,
                                            .debugName = "Cube with 4 faces"})));
        CHECK(refused(device.createTexture({.width = 4,
                                            .height = 4,
                                            .format = Format::D32Float,
                                            .usage = TextureUsage::Sampled,
                                            .debugName = "Depth without DepthStencil"})));
        CHECK(refused(device.createTexture({.width = 4,
                                            .height = 4,
                                            .format = Format::R8G8B8A8Unorm,
                                            .usage = TextureUsage::DepthStencil,
                                            .debugName = "DepthStencil without depth"})));
        CHECK(refused(device.createTexture({.width = 4,
                                            .height = 4,
                                            .depth = 2,
                                            .format = Format::R8Unorm,
                                            .usage = TextureUsage::Sampled,
                                            .debugName = "2D with depth"})));
        CHECK(refused(device.createSampler({.maxAnisotropy = 32.0f, .debugName = "Anisotropy"})));
        CHECK(refused(device.createSampler({.minLod = 2.0f, .maxLod = 1.0f, .debugName = "Lod"})));
        CHECK(refused(device.createSwapchain({.width = 0, .height = 8, .debugName = "Zero"})));
        CHECK(refused(
            device.createSwapchain({.width = 8, .height = 8, .format = Format::R32Float, .debugName = "Format"})));
        CHECK(gpu.liveObjectCount() == 0);
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == expectedAsserts);
#endif
    }

    TEST_CASE("textures with full mip chains, cubes, 3D and depth are accepted") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        CHECK(device
                  .createTexture({.width = 256,
                                  .height = 64,
                                  .mipLevels = 9,
                                  .format = Format::R8G8B8A8Srgb,
                                  .usage = TextureUsage::Sampled | TextureUsage::CopyDst,
                                  .debugName = "Full chain"})
                  .hasValue());
        CHECK(device
                  .createTexture({.dimension = TextureDimension::TextureCube,
                                  .width = 32,
                                  .height = 32,
                                  .layers = 12,
                                  .format = Format::R16G16B16A16Float,
                                  .usage = TextureUsage::Sampled,
                                  .debugName = "Two cubes"})
                  .hasValue());
        CHECK(device
                  .createTexture({.dimension = TextureDimension::Texture3D,
                                  .width = 16,
                                  .height = 16,
                                  .depth = 16,
                                  .format = Format::R32Float,
                                  .usage = TextureUsage::Storage,
                                  .debugName = "Volume"})
                  .hasValue());
        CHECK(device
                  .createTexture({.width = 64,
                                  .height = 64,
                                  .format = Format::D32Float,
                                  .usage = TextureUsage::DepthStencil | TextureUsage::Sampled,
                                  .debugName = "Depth"})
                  .hasValue());
    }

    TEST_CASE("a full table fails with OutOfHandles") {
        NullGpu gpu;
        Capabilities capabilities = ith::rhi::test::testCapabilities();
        capabilities.limits.buffers = 2;
        Device device = makeDevice(gpu, capabilities);
        makeBuffer(device);
        makeBuffer(device);
        const Expected<BufferHandle, Error> third =
            device.createBuffer({.size = 4, .usage = BufferUsage::ShaderRead, .debugName = "Third"});
        REQUIRE_FALSE(third.hasValue());
        CHECK(third.error().code == ErrorCode::OutOfHandles);
        CHECK(third.error().message.find("limit 2") != std::string::npos);
    }

    TEST_CASE("an allocation failure is reported and frees the slot and descriptors") {
        NullGpu gpu;
        Capabilities capabilities = ith::rhi::test::testCapabilities();
        capabilities.limits.buffers = 1;
        capabilities.limits.resourceDescriptors = 1;
        Device device = makeDevice(gpu, capabilities);
        gpu.failNextCreate();
        const Expected<BufferHandle, Error> failed =
            device.createBuffer({.size = 4, .usage = BufferUsage::ShaderRead, .debugName = "Huge"});
        REQUIRE_FALSE(failed.hasValue());
        CHECK(failed.error().code == ErrorCode::OutOfMemory);
        CHECK(failed.error().message.find("'Huge'") != std::string::npos);
        CHECK_FALSE(device.isLost());
        const BufferHandle buffer = makeBuffer(device); // the only slot and descriptor are free again
        CHECK(device.shaderReadIndex(buffer) == 0);
    }
}

TEST_SUITE("rhi.pipelines") {
    TEST_CASE("graphics and compute pipelines are created from shaders") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const PipelineHandle graphics = ith::rhi::test::makeGraphicsPipeline(device);
        const PipelineHandle compute = ith::rhi::test::makeComputePipeline(device);
        CHECK(device.isValid(graphics));
        CHECK(device.isValid(compute));
        CHECK(graphics != compute);

        // A depth-only pipeline has no pixel shader and no color target.
        const std::array depthOnly{ShaderDesc{.stage = ShaderStage::Vertex, .code = kShaderCode}};
        CHECK(device
                  .createGraphicsPipeline({.shaders = depthOnly,
                                           .depthFormat = Format::D32Float,
                                           .depth = {.test = true, .write = true},
                                           .debugName = "Shadow"})
                  .hasValue());
    }

    TEST_CASE("a shader with more than 128 bytes of constants is rejected") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<PipelineHandle, Error> compute = device.createComputePipeline(
            {.shader = {.stage = ShaderStage::Compute, .code = kShaderCode, .entryPoint = "cull", .constantsSize = 132},
             .debugName = "Culling"});
        REQUIRE_FALSE(compute.hasValue());
        CHECK(compute.error().code == ErrorCode::InvalidShader);
        CHECK(compute.error().message.find("'cull'") != std::string::npos);
        CHECK(device
                  .createComputePipeline(
                      {.shader = {.stage = ShaderStage::Compute, .code = kShaderCode, .constantsSize = 128},
                       .debugName = "At the limit"})
                  .hasValue());
    }

    TEST_CASE("pipelines with a wrong set of stages are refused") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const std::array targets{ColorTarget{.format = Format::R8G8B8A8Unorm}};
        const std::array pixelOnly{ShaderDesc{.stage = ShaderStage::Pixel, .code = kShaderCode}};
        const std::array twoVertex{ShaderDesc{.stage = ShaderStage::Vertex, .code = kShaderCode},
                                   ShaderDesc{.stage = ShaderStage::Vertex, .code = kShaderCode}};
        const std::array noCode{ShaderDesc{.stage = ShaderStage::Vertex}};
        const std::array twoPixel{ShaderDesc{.stage = ShaderStage::Vertex, .code = kShaderCode},
                                  ShaderDesc{.stage = ShaderStage::Pixel, .code = kShaderCode},
                                  ShaderDesc{.stage = ShaderStage::Pixel, .code = kShaderCode}};
        const ith::test::AssertCapture capture;
        for (const std::span<const ShaderDesc> shaders :
             {std::span<const ShaderDesc>(pixelOnly), std::span<const ShaderDesc>(twoVertex),
              std::span<const ShaderDesc>(noCode), std::span<const ShaderDesc>(twoPixel)}) {
            const Expected<PipelineHandle, Error> pipeline =
                device.createGraphicsPipeline({.shaders = shaders, .colorTargets = targets, .debugName = "Wrong"});
            REQUIRE_FALSE(pipeline.hasValue());
            CHECK(pipeline.error().code == ErrorCode::InvalidUsage);
        }
        const Expected<PipelineHandle, Error> compute = device.createComputePipeline(
            {.shader = {.stage = ShaderStage::Pixel, .code = kShaderCode}, .debugName = "Not compute"});
        CHECK_FALSE(compute.hasValue());
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 5);
#endif
    }
}

TEST_SUITE("rhi.limits") {
    TEST_CASE("resources beyond the size limits are refused") {
        NullGpu gpu;
        Capabilities capabilities = ith::rhi::test::testCapabilities();
        capabilities.limits.maxBufferSize = 1024;
        capabilities.limits.maxTextureDimension2D = 256;
        capabilities.limits.maxTextureDimension3D = 32;
        capabilities.limits.maxTextureLayers = 8;
        Device device = makeDevice(gpu, capabilities);
        const ith::test::AssertCapture capture;
        const auto refused = [](const auto& result) {
            return !result.hasValue() && result.error().code == ErrorCode::InvalidUsage;
        };
        CHECK(refused(device.createBuffer({.size = 1025, .usage = BufferUsage::ShaderRead, .debugName = "Big"})));
        CHECK(refused(device.createTexture({.width = 257,
                                            .height = 4,
                                            .format = Format::R8Unorm,
                                            .usage = TextureUsage::Sampled,
                                            .debugName = "Wide"})));
        CHECK(refused(device.createTexture({.width = 4,
                                            .height = 4,
                                            .layers = 9,
                                            .format = Format::R8Unorm,
                                            .usage = TextureUsage::Sampled,
                                            .debugName = "Many layers"})));
        CHECK(refused(device.createTexture({.dimension = TextureDimension::Texture3D,
                                            .width = 4,
                                            .height = 4,
                                            .depth = 33,
                                            .format = Format::R8Unorm,
                                            .usage = TextureUsage::Sampled,
                                            .debugName = "Deep"})));
        CHECK(device.createBuffer({.size = 1024, .usage = BufferUsage::ShaderRead, .debugName = "At the limit"})
                  .hasValue());
        CHECK(device
                  .createTexture({.width = 256,
                                  .height = 256,
                                  .layers = 8,
                                  .format = Format::R8Unorm,
                                  .usage = TextureUsage::Sampled,
                                  .debugName = "At the limit"})
                  .hasValue());
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 4);
#endif
    }

    TEST_CASE("the null backend reports a mapped buffer it can't allocate as out of memory") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<BufferHandle, Error> huge = device.createBuffer({.size = kNullMaxMappedSize + 1,
                                                                        .usage = BufferUsage::CopySrc,
                                                                        .memory = MemoryType::Upload,
                                                                        .debugName = "Huge upload"});
        REQUIRE_FALSE(huge.hasValue());
        CHECK(huge.error().code == ErrorCode::OutOfMemory);
        CHECK(gpu.liveObjectCount() == 0);
        // A GPU-only buffer of that size needs no memory in the null backend.
        CHECK(
            device.createBuffer({.size = kNullMaxMappedSize + 1, .usage = BufferUsage::ShaderRead, .debugName = "Big"})
                .hasValue());
    }

    TEST_CASE("a slot whose generations are used up is retired, never reused") {
        using Table = ith::rhi::detail::SlotTable<int>;
        Table table(1, Table::kRetireGeneration - 4);
        std::optional<std::uint32_t> slot = table.allocate();
        REQUIRE(slot.has_value());
        const BufferHandle first = table.handle<BufferTag>(*slot);
        CHECK(table.isLive(first));
        table.invalidate(*slot);
        table.release(*slot);
        CHECK_FALSE(table.isLive(first));

        slot = table.allocate(); // the last generation below the limit
        REQUIRE(slot.has_value());
        const BufferHandle second = table.handle<BufferTag>(*slot);
        CHECK(second.generation() == Table::kRetireGeneration - 1);
        CHECK_FALSE(table.reissue(*slot)); // reissuing would reach the limit
        CHECK(table.isLive(second));
        table.invalidate(*slot);
        table.release(*slot); // retired
        CHECK_FALSE(table.allocate().has_value());
        CHECK_FALSE(table.isLive(second));
    }

    TEST_CASE("slot sizes match the up-front cost documented in Limits") {
        if constexpr (sizeof(void*) == 8) {
            CHECK(sizeof(ith::rhi::detail::SlotTable<ith::rhi::detail::BufferSlot>::Slot) == 40);
            CHECK(sizeof(ith::rhi::detail::SlotTable<ith::rhi::detail::TextureSlot>::Slot) == 36);
        }
    }
}
