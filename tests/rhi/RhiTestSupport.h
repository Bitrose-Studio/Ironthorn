#pragma once

// Helpers for the RHI tests, which run on the null backend.

#include "ith/rhi/Device.h"
#include "ith/rhi/NullDevice.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace ith::rhi::test {

// Bytes that stand in for compiled shader code; the null backend accepts any.
inline constexpr std::array<std::byte, 4> kShaderCode{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

// Capabilities with small limits, so that the many devices of the tests are quick to create in
// Debug and sanitizer builds. Tests change them as they need.
inline Capabilities testCapabilities() {
    Capabilities capabilities;
    capabilities.limits = {.resourceDescriptors = 1024,
                           .samplerDescriptors = 64,
                           .buffers = 256,
                           .textures = 256,
                           .samplers = 64,
                           .pipelines = 64,
                           .swapchains = 4};
    return capabilities;
}

// A null device on `gpu`, with `capabilities` (limits and optional features).
inline Device makeDevice(NullGpu& gpu, const Capabilities& capabilities = testCapabilities()) {
    Expected<Device, Error> device = createNullDevice({.capabilities = capabilities, .gpu = &gpu});
    REQUIRE(device.hasValue());
    return std::move(device).value();
}

inline BufferHandle makeBuffer(Device& device, BufferUsage usage = BufferUsage::ShaderRead,
                               MemoryType memory = MemoryType::GpuOnly, std::uint64_t size = 256) {
    Expected<BufferHandle, Error> buffer =
        device.createBuffer({.size = size, .usage = usage, .memory = memory, .debugName = "Test buffer"});
    REQUIRE(buffer.hasValue());
    return *buffer;
}

inline TextureHandle makeTexture(Device& device, TextureUsage usage = TextureUsage::Sampled,
                                 Format format = Format::R8G8B8A8Unorm, std::uint32_t size = 64) {
    Expected<TextureHandle, Error> texture = device.createTexture(
        {.width = size, .height = size, .format = format, .usage = usage, .debugName = "Test texture"});
    REQUIRE(texture.hasValue());
    return *texture;
}

inline PipelineHandle makeGraphicsPipeline(Device& device) {
    const std::array shaders{ShaderDesc{.stage = ShaderStage::Vertex, .code = kShaderCode},
                             ShaderDesc{.stage = ShaderStage::Pixel, .code = kShaderCode}};
    const std::array targets{ColorTarget{.format = Format::R8G8B8A8Unorm}};
    Expected<PipelineHandle, Error> pipeline =
        device.createGraphicsPipeline({.shaders = shaders, .colorTargets = targets, .debugName = "Test graphics"});
    REQUIRE(pipeline.hasValue());
    return *pipeline;
}

inline PipelineHandle makeComputePipeline(Device& device) {
    Expected<PipelineHandle, Error> pipeline = device.createComputePipeline(
        {.shader = {.stage = ShaderStage::Compute, .code = kShaderCode}, .debugName = "Test compute"});
    REQUIRE(pipeline.hasValue());
    return *pipeline;
}

// Begins a frame that must be drawn.
inline Frame beginFrame(Device& device, SwapchainHandle swapchain = {}) {
    Expected<std::optional<Frame>, Error> frame = device.beginFrame(swapchain);
    REQUIRE(frame.hasValue());
    REQUIRE(frame->has_value());
    return **frame;
}

// Ends a frame that must succeed.
inline void endFrame(Device& device) {
    const Expected<void, Error> ended = device.endFrame();
    REQUIRE(ended.hasValue());
}

// One frame with nothing in it.
inline void emptyFrame(Device& device) {
    beginFrame(device);
    endFrame(device);
}

} // namespace ith::rhi::test
