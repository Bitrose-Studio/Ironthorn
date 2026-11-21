#pragma once

// Bit operations for the RHI's flag enums (usages, pipeline stages, accesses).
//
//   enum class BufferUsage : std::uint8_t { None = 0, ShaderRead = 1u << 0, ... };
//   template <>
//   inline constexpr bool kIsFlags<BufferUsage> = true;
//
//   const BufferUsage usage = BufferUsage::ShaderRead | BufferUsage::CopyDst;
//   if (hasAny(usage, BufferUsage::CopyDst)) { ... }

#include <type_traits>

namespace ith::rhi {

// True for the scoped enums whose values are bits; each flag enum opts in.
template <typename Enum>
inline constexpr bool kIsFlags = false;

template <typename Enum>
    requires kIsFlags<Enum>
[[nodiscard]] constexpr Enum operator|(Enum a, Enum b) noexcept {
    using Bits = std::underlying_type_t<Enum>;
    return static_cast<Enum>(static_cast<Bits>(a) | static_cast<Bits>(b));
}

template <typename Enum>
    requires kIsFlags<Enum>
[[nodiscard]] constexpr Enum operator&(Enum a, Enum b) noexcept {
    using Bits = std::underlying_type_t<Enum>;
    return static_cast<Enum>(static_cast<Bits>(a) & static_cast<Bits>(b));
}

// True when `value` has at least one of the bits of `bits`.
template <typename Enum>
    requires kIsFlags<Enum>
[[nodiscard]] constexpr bool hasAny(Enum value, Enum bits) noexcept {
    return static_cast<std::underlying_type_t<Enum>>(value & bits) != 0;
}

} // namespace ith::rhi
