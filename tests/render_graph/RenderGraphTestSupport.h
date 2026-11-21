#pragma once

// Helpers for the render graph tests, which compile without a device and execute on the RHI's
// null backend.

#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/NullDevice.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ith::rg::test {

// An RHI handle for graphs that are only compiled: compile never looks at it.
inline rhi::TextureHandle fakeTexture(std::uint32_t index) {
    return rhi::TextureHandle::fromParts(index, 1);
}

inline rhi::BufferHandle fakeBuffer(std::uint32_t index) {
    return rhi::BufferHandle::fromParts(index, 1);
}

// A 2D transient texture.
inline TextureDesc texture(std::string_view name, rhi::Format format = rhi::Format::R16G16B16A16Float,
                           std::uint32_t size = 64) {
    return {.width = size, .height = size, .format = format, .debugName = name};
}

// The description of a swapchain-like 64x64 image for TextureImport.
inline rhi::TextureDesc backbufferDesc(std::uint32_t size = 64) {
    return {.width = size,
            .height = size,
            .format = rhi::Format::B8G8R8A8Srgb,
            .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopyDst,
            .debugName = "Backbuffer"};
}

// An execute callable that records nothing.
inline auto nothing() {
    return [](PassContext& /*context*/) {};
}

// A null device with small limits, on `gpu`.
inline rhi::Device makeDevice(rhi::NullGpu& gpu) {
    rhi::Capabilities capabilities;
    capabilities.limits = {.resourceDescriptors = 1024,
                           .samplerDescriptors = 64,
                           .buffers = 256,
                           .textures = 256,
                           .samplers = 64,
                           .pipelines = 64,
                           .swapchains = 4};
    Expected<rhi::Device, rhi::Error> device = rhi::createNullDevice({.capabilities = capabilities, .gpu = &gpu});
    REQUIRE(device.hasValue());
    return std::move(device).value();
}

// Begins a frame that must be drawn.
inline rhi::Frame beginFrame(rhi::Device& device, rhi::SwapchainHandle swapchain = {}) {
    Expected<std::optional<rhi::Frame>, rhi::Error> frame = device.beginFrame(swapchain);
    REQUIRE(frame.hasValue());
    REQUIRE(frame->has_value());
    return **frame;
}

inline void endFrame(rhi::Device& device) {
    const Expected<void, rhi::Error> ended = device.endFrame();
    REQUIRE(ended.hasValue());
}

// Executes a compiled graph in a frame of its own.
inline void executeFrame(rhi::Device& device, RenderGraph& graph, ResourcePool& pool, JobSystem* jobs = nullptr) {
    beginFrame(device);
    const Expected<void, rhi::Error> executed = graph.execute(pool, jobs);
    CHECK_MESSAGE(executed.hasValue(), (executed ? std::string() : executed.error().toString()));
    endFrame(device);
}

} // namespace ith::rg::test
