#include "ith/asset_database/Hash128.h"

// The only file that includes xxHash: XXH_INLINE_ALL makes every xxHash symbol static here.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <array>

namespace ith {

std::string Hash128::toString() const {
    constexpr std::array<char, 16> kDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string text(32, '0');
    for (std::size_t index = 0; index < 16; ++index) {
        const auto shift = static_cast<unsigned>(60 - index * 4);
        text[index] = kDigits[(high >> shift) & 0xf];
        text[16 + index] = kDigits[(low >> shift) & 0xf];
    }
    return text;
}

Hash128 hash128(std::span<const std::byte> bytes) noexcept {
    // XXH3-128 with the default secret and seed 0. Its output is frozen across xxHash releases
    // and the same on every platform.
    const XXH128_hash_t hash = XXH3_128bits(bytes.data(), bytes.size());
    return Hash128{.high = hash.high64, .low = hash.low64};
}

Hash128 hash128(std::string_view text) noexcept {
    return hash128(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

} // namespace ith
