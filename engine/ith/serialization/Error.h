#pragma once

// serialization::Error: what went wrong while reading or writing data, and where (ADR-0010).
//
//   const ith::Expected<void, ith::serialization::Error> result = ith::serialization::readText(...);
//   if (!result) {
//       log(result.error().toString()); // "level.json: $.items[3].count: ... [serialization.wrong_type]"
//   }
//
// The same type describes warnings (unknown fields read in the editor), which are collected
// instead of failing the read.

#include <cstddef>
#include <optional>
#include <string>

namespace ith::serialization {

struct Error {
    // Stable dotted identifier, e.g. "serialization.wrong_type". Never localized.
    std::string code;
    // What went wrong, for people.
    std::string message;
    // The file or other source name the caller passed in; empty when none was given.
    std::string source;
    // Where in the data: "$" is the document's data, ".name" a field, "[3]" an element, e.g.
    // "$.loadout.items[3].count". Empty when the problem is outside the data (a header or the
    // syntax of the whole file).
    std::string path;
    // 1-based line and column of a text syntax error; 0 when not known.
    std::size_t line = 0;
    std::size_t column = 0;
    // Byte offset in the input: always set for binary data, and for text syntax errors.
    std::optional<std::size_t> offset;

    // One line for logs: "<source>:<line>:<column>: <path>: <message> [<code>]", leaving out
    // the parts that are not known. Binary offsets are written as "@<offset>".
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const Error&, const Error&) = default;
};

} // namespace ith::serialization
