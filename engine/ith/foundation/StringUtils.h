#pragma once

// Small, locale-independent string helpers.
//
// Text is treated as UTF-8, but only ASCII characters are classified or changed: every byte
// of a multi-byte sequence is above 0x7F and passes through untouched. None of the functions
// reads the C locale, so results are the same on every platform and thread.
//
//   for (const std::string_view part : ith::split("a, b,,c", ',')) {
//       const std::string_view word = ith::trim(part); // "a", "b", "", "c"
//   }
//   ith::equalsIgnoreCaseAscii("Space", "SPACE");     // true
//   ith::replaceAll("a\\b\\c", "\\", "/");       // "a/b/c"
//
// All functions are thread-safe: they read only their arguments.

#include "ith/foundation/Compiler.h"

#include <string>
#include <string_view>
#include <vector>

namespace ith {

// Returns `text` without leading and trailing ASCII whitespace (space, \t, \n, \v, \f, \r).
// The result is a view into `text`.
std::string_view trim(std::string_view text ITH_LIFETIMEBOUND);

// Splits `text` at every `delimiter`. Empty parts are kept, so the result always has one more
// element than there are delimiters: split("a,,b", ',') is {"a", "", "b"} and split("", ',')
// is {""}. The views point into `text`.
std::vector<std::string_view> split(std::string_view text ITH_LIFETIMEBOUND, char delimiter);

// Returns a copy of `text` with the ASCII letters A-Z lowercased. Every other byte is copied
// unchanged.
std::string toLowerAscii(std::string_view text);

// Compares two strings, treating ASCII letters of either case as equal. Other bytes must match
// exactly.
bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b);

// Returns a copy of `text` with every non-overlapping occurrence of `from` replaced by `to`,
// scanning left to right. Precondition (asserted): `from` is not empty. If it is, `text` is
// returned unchanged.
std::string replaceAll(std::string_view text, std::string_view from, std::string_view to);

// Returns the value (0-15) of a hexadecimal digit in either letter case, or -1 when `character`
// is not a hexadecimal digit.
constexpr int hexDigitValue(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace ith
