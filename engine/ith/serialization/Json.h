#pragma once

// Reading and writing JSON text as Value trees (ADR-0010).
//
//   const ith::Expected<ith::serialization::Value, ith::serialization::Error> config =
//       ith::serialization::parseJson(text, {.source = "engine.json", .syntax = ith::serialization::JsonSyntax::Config});
//   const ith::Expected<std::string, ith::serialization::Error> written = ith::serialization::writeJson(*config);
//
// The reader accepts RFC 8259 JSON in UTF-8: no byte order mark, no trailing commas, no
// infinities or NaN, and comments only in config files. It refuses documents larger than
// kMaxTextSize (or a lower limit the caller sets), nested deeper than kMaxDepth, or whose tree would
// take more than the allocation budget (Document.h). The writer prints two-space indented JSON with
// LF line endings and a newline at the end, keeps object members in order, and writes numbers
// exactly as their text.
//
// Both functions are thread-safe and allocate only through the module's allocator.

#include "ith/foundation/Expected.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Error.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ith::serialization {

enum class JsonSyntax : std::uint8_t {
    Strict, // RFC 8259 only: assets, scenes and everything the editor writes
    Config, // also /* */ and // comments: hand-edited config files only
};

struct JsonReadOptions {
    // The file name or other source, copied into errors.
    std::string_view source;
    JsonSyntax syntax = JsonSyntax::Strict;
    // The largest document accepted, in bytes; at most kMaxTextSize. Config files can set less.
    std::size_t maxSize = kMaxTextSize;
    // The bytes the value tree may take; 0 means the default of allocationBudget (Document.h).
    std::size_t allocationBudget = 0;
};

// Parses one JSON document. Fails with "serialization.too_large" (the document or its tree is too
// big), "serialization.syntax" (with line, column and byte offset; this includes invalid UTF-8)
// or "serialization.too_deep".
Expected<Value, Error> parseJson(std::string_view text, const JsonReadOptions& options = {});

// Writes `value` as pretty JSON. Fails with "serialization.too_deep", "serialization.too_large",
// or "serialization.write_failed" for a string that is not valid UTF-8.
Expected<std::string, Error> writeJson(const Value& value);

} // namespace ith::serialization
