#pragma once

// The text archive: any reflected value as a JSON document (ADR-0010, ADR-0020). Source data (assets,
// scenes, config) is saved this way.
//
//   constexpr ith::serialization::DocumentFormat kLoadoutFormat{.name = "game.loadout", .version = 1};
//
//   const auto text = ith::serialization::writeText(kLoadoutFormat, loadout);
//   // {
//   //   "format": "game.loadout",
//   //   "version": 1,
//   //   "data": {
//   //     "slots": ["Fire", "Poison"],
//   //     ...
//   //   }
//   // }
//
//   std::vector<ith::serialization::Error> warnings;
//   const auto read = ith::serialization::readText(*text, kLoadoutFormat, loadout, {.source = "a.json", .warnings = &warnings});
//
// The document is an object with "format", "version" and "data"; "data" holds the value:
// - bool as true or false; integers as exact decimal numbers; float and double as the shortest
//   text that reads back bit-identical (std::to_chars), never infinities or NaN;
// - std::string as a string, ith::Uuid as its canonical string;
// - an enum as the name of its enumerator; a Flags enum as an array of enumerator names whose
//   values are ORed together;
// - a struct as an object of its fields in declaration order, without Transient fields.
//   EditorOnly fields are written: text is source data. SaveGame has no effect yet;
// - std::array and std::vector as arrays, std::optional as null or its value. An optional of
//   an optional can't be told apart from an empty one and is refused.
//
// Reading is strict about values and lenient about fields: a missing field keeps its default,
// a field may be named by its FormerName, and an unknown field is a warning and is dropped
// (UnknownFields::Fail turns it into an error, for cooking). A read builds the whole value first
// and only then replaces the target, so a failed read leaves the target unchanged. A successful
// read replaces the whole object: Transient and unreflected members get their default values,
// and a container holds exactly the elements the document lists. Reads allocate at most the
// allocation budget (Document.h).
//
// All functions are thread-safe for distinct targets.

#include "ith/foundation/Expected.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Error.h"
#include "ith/serialization/Json.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ith::serialization {

// What a reader does with a field the type doesn't have.
enum class UnknownFields : std::uint8_t {
    Warn, // report a "serialization.unknown_field" warning and drop the field (the editor)
    Fail, // fail with "serialization.unknown_field" (cooking, so stale data never ships)
};

struct TextReadOptions {
    // The file name or other source, copied into errors and warnings.
    std::string_view source;
    // JsonSyntax::Config allows comments; use it for hand-edited config files only.
    JsonSyntax syntax = JsonSyntax::Strict;
    UnknownFields unknownFields = UnknownFields::Warn;
    // Receives the warnings of a successful read, appended: at most 100 unknown fields, then one
    // warning that counts the rest. Null to not collect them.
    std::vector<Error>* warnings = nullptr;
    // The largest document accepted, in bytes; at most kMaxTextSize. Config files can set less.
    std::size_t maxSize = kMaxTextSize;
    // The bytes the read may allocate; 0 means the default of allocationBudget (Document.h). The
    // JSON tree and the value are each checked against it.
    std::size_t allocationBudget = 0;
};

// Writes the value of `type` at `object` as a document of `format` at its current version.
// Fails with "serialization.non_finite" (an infinite or NaN float), "serialization.invalid_enum"
// (a value that is no enumerator, or has bits no enumerator has), "serialization.not_saved" (the
// type, or an element type, is Transient), "serialization.unsupported_type" (an optional of an
// optional), "serialization.too_deep", "serialization.too_large" or "serialization.write_failed"
// (a string that is not UTF-8). Precondition (asserted): `format` is valid (Document.h).
Expected<std::string, Error> writeText(const DocumentFormat& format, const TypeInfo& type, const void* object);

// Reads a document of `format` into the value of `type` at `target`, migrating older versions.
// On failure the target is unchanged. Fails with the parseJson errors and:
// - "serialization.not_a_document": not an object with "format", "version" and "data";
// - "serialization.wrong_format", "serialization.newer_version", "serialization.version_too_old";
// - "serialization.migration_failed";
// - "serialization.wrong_type", "serialization.out_of_range", "serialization.unknown_enumerator",
//   "serialization.invalid_uuid", "serialization.wrong_size" (a std::array of another length);
// - "serialization.duplicate_field": a field given twice, also under its FormerName;
// - "serialization.unknown_field" with UnknownFields::Fail;
// - "serialization.not_saved", "serialization.unsupported_type" as for writeText;
// - "serialization.too_large": the document, its tree or the value exceeds the limits.
// Preconditions: `format` is valid (asserted); `target` is a complete, non-const object of `type`.
Expected<void, Error> readText(std::string_view text, const DocumentFormat& format, const TypeInfo& type, void* target,
                               const TextReadOptions& options = {});

// writeText for a reflected type T.
template <typename T>
Expected<std::string, Error> writeText(const DocumentFormat& format, const T& value) {
    return writeText(format, typeOf<T>(), &value);
}

// readText for a reflected type T.
template <typename T>
Expected<void, Error> readText(std::string_view text, const DocumentFormat& format, T& target,
                               const TextReadOptions& options = {}) {
    return readText(text, format, typeOf<T>(), &target, options);
}

} // namespace ith::serialization
