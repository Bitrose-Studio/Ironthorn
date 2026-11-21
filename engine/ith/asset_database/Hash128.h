#pragma once

// 128-bit content hashing for derived-data cache keys (ADR-0015 rule 11).
//
//   const ith::Hash128 sourceHash = ith::hash128(sourceBytes);
//   const std::string name = sourceHash.toString(); // 32 lowercase hexadecimal digits
//
// The function is XXH3-128 (xxHash, default secret, seed 0), whose output is the same on every
// platform and frozen across xxHash releases. Callers only rely on equal input giving an equal
// hash and different input giving a different hash with overwhelming probability. Hashes name
// entries in the local cache, which is never committed, so a change of function only makes every
// existing entry a miss. It is tools-only for now; it moves to foundation when a runtime module
// needs content hashes.
//
// All functions are pure and thread-safe.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ith {

// A 128-bit hash value.
struct Hash128 {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    // The 32 lowercase hexadecimal digits of high, then low.
    [[nodiscard]] std::string toString() const;

    constexpr auto operator<=>(const Hash128&) const noexcept = default;
};

// The 128-bit hash of `bytes`.
[[nodiscard]] Hash128 hash128(std::span<const std::byte> bytes) noexcept;

// The 128-bit hash of the bytes of `text`; equal to hash128 over the same bytes.
[[nodiscard]] Hash128 hash128(std::string_view text) noexcept;

} // namespace ith
