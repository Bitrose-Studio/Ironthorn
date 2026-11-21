#pragma once

// Typed generational handles to RHI objects (ADR-0009 rule 4).
//
//   ith::rhi::BufferHandle vertices = *device.createBuffer(desc);
//   device.destroy(vertices); // `vertices` is stale from now on
//
// A handle is 64 bits: the index of a slot in the device's table of that object type and the
// slot's generation. Destroying an object raises its slot's generation at once, so every copy of
// the handle becomes stale and never refers to a later object in the same slot; the slot itself
// is reused only after deferred destruction has released the object (rule 5). Using a stale
// handle is asserted in Debug and development builds. Generations start at 1, so the default
// handle (generation 0) is the null handle of every type.
//
// Handles are plain values: trivially copyable and comparable. They belong to the device that
// created them and are never saved or sent anywhere.

#include <cstdint>

namespace ith::rhi {

template <typename Tag>
class Handle {
public:
    // The null handle; refers to no object.
    constexpr Handle() noexcept = default;

    // A handle from its parts. The device creates handles; backends and tests use this.
    [[nodiscard]] static constexpr Handle fromParts(std::uint32_t index, std::uint32_t generation) noexcept {
        Handle handle;
        handle.m_index = index;
        handle.m_generation = generation;
        return handle;
    }

    // The slot in the device's table; also the index a backend keeps its native object at.
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return m_index; }

    // How many times the slot was used before, plus one; 0 only for the null handle.
    [[nodiscard]] constexpr std::uint32_t generation() const noexcept { return m_generation; }

    [[nodiscard]] constexpr bool isNull() const noexcept { return m_generation == 0; }

    friend constexpr bool operator==(Handle a, Handle b) noexcept = default;

private:
    std::uint32_t m_index = 0;
    std::uint32_t m_generation = 0;
};

struct BufferTag;
struct TextureTag;
struct SamplerTag;
struct PipelineTag;
struct SwapchainTag;

// A GPU buffer (BufferDesc).
using BufferHandle = Handle<BufferTag>;
// A texture (TextureDesc), including a swapchain's images.
using TextureHandle = Handle<TextureTag>;
// A sampler (SamplerDesc).
using SamplerHandle = Handle<SamplerTag>;
// A graphics or compute pipeline (Pipeline.h).
using PipelineHandle = Handle<PipelineTag>;
// A swapchain of a window (SwapchainDesc).
using SwapchainHandle = Handle<SwapchainTag>;

// The value of a bindless descriptor index that refers to nothing.
inline constexpr std::uint32_t kInvalidDescriptorIndex = 0xFFFF'FFFFu;

} // namespace ith::rhi
