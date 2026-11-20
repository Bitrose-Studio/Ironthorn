#pragma once

// Non-cryptographic 64-bit hashing.
//
//   constexpr std::uint64_t kRenderId = ith::fnv1a64("render"); // usable at compile time
//   const std::uint64_t key = ith::hashCombine(ith::fnv1a64(meshName), materialIndex);
//
// fnv1a64 is the FNV-1a algorithm, so its results are the same on every platform, compiler and
// engine version and may be stored on disk. It is meant for identifiers, lookup tables and short
// keys; it is not a defence against adversarial input and not a content digest for large data.
//
// All functions are pure and thread-safe.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ith {

// The FNV-1a 64-bit offset basis: the hash of no bytes.
inline constexpr std::uint64_t kFnv1a64Basis = 0xcbf29ce484222325ull;

// The FNV-1a 64-bit prime.
inline constexpr std::uint64_t kFnv1a64Prime = 0x100000001b3ull;

// FNV-1a over the bytes of `text`. Passing the result of a previous call as `basis` continues
// the hash, so fnv1a64("ab") == fnv1a64("b", fnv1a64("a")).
constexpr std::uint64_t fnv1a64(std::string_view text, std::uint64_t basis = kFnv1a64Basis) noexcept {
    std::uint64_t hash = basis;
    for (const char character : text) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

// FNV-1a over raw bytes. Equal to fnv1a64(std::string_view) over the same bytes.
constexpr std::uint64_t fnv1a64(std::span<const std::byte> bytes, std::uint64_t basis = kFnv1a64Basis) noexcept {
    std::uint64_t hash = basis;
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

// Mixes `value` into the running hash `seed` and returns the new hash. The result depends on
// the order of the values: hashCombine(hashCombine(s, a), b) differs from the reverse order in
// general. Stable across platforms.
constexpr std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4));
}

} // namespace ith
