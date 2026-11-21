#pragma once

// The tokenizer of the ith_reflect generator.
//
// It splits a header into identifiers, numbers, string and character literals and punctuation,
// and drops whitespace, comments and preprocessor lines. It understands no more C++ than that
// (ADR-0018 rule 3). Each token records its line and how many #if blocks it is inside, so the
// parser can refuse annotations in conditional code.

#include "ith/foundation/Expected.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ith::reflect {

// A generation error at a place in a header.
struct Diagnostic {
    std::string file;
    int line = 0;
    std::string message;

    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

// "file:line: error: message", the form IDEs and build logs link to the source.
std::string formatDiagnostic(const Diagnostic& diagnostic);

enum class TokenKind : std::uint8_t {
    Identifier,  // also keywords
    Number,      // a preprocessing number: 10, 0.5f, 1'000, 0x1p-3
    String,      // a string literal with its prefix and quotes, e.g. "text" or u8"text"
    Character,   // a character literal with its quotes
    Punctuation, // one character, except "::"
};

struct Token {
    TokenKind kind = TokenKind::Punctuation;
    std::string_view text; // a view into the header text
    int line = 0;
    int conditionalDepth = 0; // number of enclosing #if, #ifdef and #ifndef blocks

    [[nodiscard]] bool is(std::string_view value) const { return text == value; }
};

// Splits `text`, the contents of the header at `path`, into tokens. The tokens refer to `text`,
// which must outlive them. Fails on an unterminated comment or literal and on unbalanced #if
// and #endif lines.
Expected<std::vector<Token>, Diagnostic> tokenize(std::string_view path, std::string_view text);

} // namespace ith::reflect
