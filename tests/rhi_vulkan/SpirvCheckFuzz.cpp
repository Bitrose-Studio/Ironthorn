// libFuzzer target for the Vulkan backend's SPIR-V binding check (SpirvCheck.h): runs arbitrary
// bytes through it, which must never read out of bounds or allocate more than a few times the
// input's size. Built only in Clang sanitizer builds; CTest runs a short smoke run seeded with
// tests/rhi_vulkan/fuzz_seeds (SPIR-V from the engine's Slang compiler), and longer runs are
// started by hand:
//
//   ith_rhi_vulkan_spirv_fuzz -max_total_time=600 <corpus directory> tests/rhi_vulkan/fuzz_seeds

#include "ith/rhi_vulkan/SpirvCheck.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libFuzzer hands out raw bytes
    const std::span<const std::byte> code(reinterpret_cast<const std::byte*>(data), size);
    if (ith::rhi::vulkan::isSpirv(code)) {
        static_cast<void>(ith::rhi::vulkan::bindingProblem(code, {.resources = 262'144, .samplers = 2'048}));
    }
    return 0;
}
