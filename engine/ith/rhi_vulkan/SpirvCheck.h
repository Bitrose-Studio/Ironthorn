#pragma once

// Checks that a SPIR-V module uses only the bindless binding model of the Vulkan backend
// (VulkanBackend.h). Internal to the rhi_vulkan module, and its fuzz target.
//
// This is not a SPIR-V validator: the backend takes trusted, engine-compiled SPIR-V (the shader
// compiler validates it when it is built) and runs no validator at run time. The check only
// makes sure that a module never names a descriptor the pipeline layout doesn't have, and that it
// reads any input, however malformed, without going out of bounds or allocating more than a few
// times the input's size.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ith::rhi::vulkan {

// The descriptor counts of the bindless set's bindings.
struct BindlessCounts {
    std::uint32_t resources = 0;
    std::uint32_t samplers = 0;
};

// Whether `code` starts like SPIR-V: whole 32-bit little-endian words after a SPIR-V header.
[[nodiscard]] bool isSpirv(std::span<const std::byte> code);

// Why the module declares a resource outside the bindless set, or std::nullopt when it doesn't.
// Every variable of a resource storage class (UniformConstant, Uniform, StorageBuffer) must be in
// set 0 with a binding and type that the set has:
//   binding 0  storage buffers (StorageBuffer storage class)
//   binding 1  sampled images (OpTypeImage with Sampled = 1)
//   binding 2  storage images (OpTypeImage with Sampled = 2)
//   binding 3  samplers (OpTypeSampler)
// each alone or in an array, sized or not; a sized array may not be longer than the binding. Push
// constants have no binding. Also refuses a module that is not well-formed enough to tell.
// Precondition: isSpirv(code).
[[nodiscard]] std::optional<std::string> bindingProblem(std::span<const std::byte> code, BindlessCounts counts);

} // namespace ith::rhi::vulkan
