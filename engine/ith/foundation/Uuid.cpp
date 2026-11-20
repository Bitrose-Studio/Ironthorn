#include "ith/foundation/Uuid.h"

#include "ith/foundation/StringUtils.h"

#include <array>
#include <random>

namespace ith {
namespace {

constexpr std::size_t kTextLength = 36;
constexpr std::uint64_t kVersionMask = 0x000000000000f000ull;
constexpr std::uint64_t kVersion4 = 0x0000000000004000ull;
constexpr std::uint64_t kVariantMask = 0xc000000000000000ull;
constexpr std::uint64_t kVariantRfc = 0x8000000000000000ull;

bool isDashPosition(std::size_t index) {
    return index == 8 || index == 13 || index == 18 || index == 23;
}

std::mt19937_64 makeSeededGenerator() {
    // 256 bits of seed spread over the generator's whole state by seed_seq.
    std::random_device device;
    std::array<std::uint32_t, 8> seedData{};
    for (std::uint32_t& word : seedData) {
        word = device();
    }
    std::seed_seq seed(seedData.begin(), seedData.end());
    return std::mt19937_64(seed);
}

} // namespace

Uuid Uuid::generate() {
    static thread_local std::mt19937_64 generator = makeSeededGenerator();
    const std::uint64_t high = generator();
    const std::uint64_t low = generator();
    return {(high & ~kVersionMask) | kVersion4, (low & ~kVariantMask) | kVariantRfc};
}

Expected<Uuid> Uuid::parse(std::string_view text) {
    if (text.size() != kTextLength) {
        return makeError("uuid.malformed", "a Uuid must be 36 characters in the form 8-4-4-4-12");
    }

    std::array<std::uint64_t, 2> halves{};
    std::size_t digitCount = 0;
    for (std::size_t index = 0; index < kTextLength; ++index) {
        if (isDashPosition(index)) {
            if (text[index] != '-') {
                return makeError("uuid.malformed", "a Uuid must have dashes in the form 8-4-4-4-12");
            }
            continue;
        }

        const int digit = hexDigitValue(text[index]);
        if (digit < 0) {
            return makeError("uuid.malformed", "a Uuid may contain only hexadecimal digits and dashes");
        }
        std::uint64_t& half = halves[digitCount / 16];
        half = (half << 4) | static_cast<std::uint64_t>(digit);
        ++digitCount;
    }
    return Uuid{halves[0], halves[1]};
}

std::string Uuid::toString() const {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text(kTextLength, '-');
    std::size_t digitIndex = 0;
    for (std::size_t index = 0; index < kTextLength; ++index) {
        if (isDashPosition(index)) {
            continue;
        }
        const std::uint64_t half = digitIndex < 16 ? m_high : m_low;
        const auto shift = static_cast<unsigned>(60 - (4 * (digitIndex % 16)));
        text[index] = kDigits[(half >> shift) & 0xfu];
        ++digitIndex;
    }
    return text;
}

} // namespace ith
