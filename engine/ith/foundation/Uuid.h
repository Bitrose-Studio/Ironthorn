#pragma once

// 128-bit universally unique identifiers (RFC 9562).
//
// A Uuid names an asset or an object independently of its path and display name, so references
// survive renames and moves.
//
//   const ith::Uuid id = ith::Uuid::generate();
//   const std::string text = id.toString(); // "3f2b8c1e-9d4a-4c1b-8e2f-0a1b2c3d4e5f"
//   const ith::Expected<ith::Uuid> parsed = ith::Uuid::parse(text);
//
// The 128 bits are stored as two 64-bit halves in the order they are written: high() holds the
// first 16 hexadecimal digits of the canonical text form and low() the last 16.

#include "ith/foundation/Expected.h"
#include "ith/foundation/Hash.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ith {

class Uuid {
public:
    // The nil Uuid (all zero bits), which means "no object".
    constexpr Uuid() noexcept = default;

    // A Uuid with the given bits, taken as they are (no version or variant is set).
    constexpr Uuid(std::uint64_t high, std::uint64_t low) noexcept : m_high(high), m_low(low) {}

    // Returns a new random Uuid (RFC 9562 version 4: 122 random bits).
    //
    // Each thread draws from its own pseudo-random generator, seeded once per thread from
    // std::random_device, so the call is thread-safe and takes no lock. The values are unique
    // for practical purposes but are not cryptographically secure: never use them as secrets.
    static Uuid generate();

    // Parses the canonical 8-4-4-4-12 hexadecimal form, in either letter case, with no braces
    // or surrounding whitespace. Fails with the error code "uuid.malformed" for anything else.
    static Expected<Uuid> parse(std::string_view text);

    // Returns the canonical lowercase 8-4-4-4-12 form (36 characters).
    [[nodiscard]] std::string toString() const;

    // True for the nil Uuid.
    [[nodiscard]] constexpr bool isNil() const noexcept { return m_high == 0 && m_low == 0; }

    // The first 64 bits (digits 1-16 of the text form).
    [[nodiscard]] constexpr std::uint64_t high() const noexcept { return m_high; }

    // The last 64 bits (digits 17-32 of the text form).
    [[nodiscard]] constexpr std::uint64_t low() const noexcept { return m_low; }

    // Two Uuids are equal when both halves are.
    //
    // Written out rather than defaulted on purpose: a defaulted == makes Uuid trivially equality
    // comparable to clang, and some standard libraries then send std::find, std::count,
    // std::ranges::unique and similar algorithms over Uuids to vectorized code that only handles
    // 1, 2, 4 and 8-byte elements, which fails to compile for 16 bytes.
    [[nodiscard]] constexpr bool operator==(const Uuid& other) const noexcept {
        return m_high == other.m_high && m_low == other.m_low;
    }

    // Uuids order by high(), then low(): the same order as their text forms.
    constexpr auto operator<=>(const Uuid&) const noexcept = default;

private:
    std::uint64_t m_high = 0;
    std::uint64_t m_low = 0;
};

} // namespace ith

// Lets Uuid be a key of std::unordered_map and std::unordered_set.
template <>
struct std::hash<ith::Uuid> {
    std::size_t operator()(const ith::Uuid& id) const noexcept {
        return static_cast<std::size_t>(ith::hashCombine(id.high(), id.low()));
    }
};
