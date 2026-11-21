#include "ith/foundation/StringUtils.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ith {
namespace {

bool isAsciiSpace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\v' || character == '\f' ||
           character == '\r';
}

char toLowerAsciiChar(char character) {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
}

} // namespace

std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && isAsciiSpace(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && isAsciiSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = text.find(delimiter, begin);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(begin));
            return parts;
        }
        parts.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
}

std::string toLowerAscii(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), toLowerAsciiChar);
    return result;
}

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b,
                              [](char left, char right) { return toLowerAsciiChar(left) == toLowerAsciiChar(right); });
}

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to) {
    ITH_ASSERT_MSG(!from.empty(), "replaceAll: the pattern to replace must not be empty");
    if (from.empty()) {
        return std::string(text);
    }

    std::string result;
    result.reserve(text.size());
    std::size_t begin = 0;
    while (true) {
        const std::size_t match = text.find(from, begin);
        if (match == std::string_view::npos) {
            result.append(text.substr(begin));
            return result;
        }
        result.append(text.substr(begin, match - begin));
        result.append(to);
        begin = match + from.size();
    }
}

bool endsWithIgnoreCaseAscii(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && equalsIgnoreCaseAscii(text.substr(text.size() - suffix.size()), suffix);
}

bool isValidUtf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t value = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
            value = lead & 0x1fu;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            value = lead & 0x0fu;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            value = lead & 0x07u;
        } else {
            return false; // a continuation byte, an overlong 2-byte lead (C0, C1) or F5 to FF
        }
        if (text.size() - index < length) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xc0u) != 0x80u) {
                return false;
            }
            value = (value << 6u) | (next & 0x3fu);
        }
        const bool overlong = (length == 3 && value < 0x800) || (length == 4 && value < 0x10000);
        if (overlong || (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            return false;
        }
        index += length;
    }
    return true;
}

} // namespace ith
