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
// The document is an object with "format", "version" and "data", in that order; an asset document
// (a native asset, ADR-0024) also has an "id" before "data". "data" holds the value:
// - bool as true or false; integers as exact decimal numbers; float and double as the shortest
//   text that reads back bit-identical (std::to_chars), never infinities or NaN;
// - std::string as a string, ith::Uuid as its canonical string, an ith::Entity as the canonical
//   string of the Uuid its entity mapping gives (Document.h, EntityMapping; refused without one);
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
#include "ith/foundation/Uuid.h"
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
    // The document's "id" (ADR-0024 rule 3: the caller chooses for each read). Not null: an asset
    // document is expected, which must have an "id" that is a UUID other than nil; the id is stored
    // here when the read succeeds. Null: a document with an "id" is refused. Either mismatch fails
    // with "serialization.not_a_document", whatever `unknownFields` says.
    Uuid* id = nullptr;
    // How Entity values are read (Document.h). Null: a document whose value holds a saved Entity
    // fails with "serialization.no_entity_mapping".
    const EntityMapping* entities = nullptr;
};

// Writes the value of `type` at `object` as a document of `format` at its current version. With
// an `id`, an asset document: the id is written between "version" and "data"; a nil id fails with
// "serialization.invalid_id". Fails with "serialization.non_finite" (an infinite or NaN float),
// "serialization.invalid_enum" (a value that is no enumerator, or has bits no enumerator has),
// "serialization.not_saved" (the type, or an element type, is Transient),
// "serialization.unsupported_type" (an optional of an optional), "serialization.too_deep",
// "serialization.too_large", "serialization.write_failed" (a string that is not UTF-8) or
// "serialization.no_entity_mapping" (the value holds a saved Entity and `entities` is null).
// Precondition (asserted): `format` is valid (Document.h).
Expected<std::string, Error> writeText(const DocumentFormat& format, const TypeInfo& type, const void* object,
                                       const Uuid* id = nullptr, const EntityMapping* entities = nullptr);

// Reads a document of `format` into the value of `type` at `target`, migrating older versions.
// On failure the target is unchanged. Fails with the parseJson errors and:
// - "serialization.not_a_document": not an object with "format", "version" and "data", or an "id"
//   that doesn't match options.id (see TextReadOptions::id);
// - "serialization.wrong_format", "serialization.newer_version", "serialization.version_too_old";
// - "serialization.migration_failed";
// - "serialization.wrong_type", "serialization.out_of_range", "serialization.unknown_enumerator",
//   "serialization.invalid_uuid", "serialization.wrong_size" (a std::array of another length);
// - "serialization.duplicate_field": a field given twice, also under its FormerName;
// - "serialization.unknown_field" with UnknownFields::Fail;
// - "serialization.not_saved", "serialization.unsupported_type" as for writeText;
// - "serialization.no_entity_mapping": the value holds a saved Entity and options.entities is null;
// - "serialization.too_large": the document, its tree or the value exceeds the limits.
// Preconditions: `format` is valid (asserted); `target` is a complete, non-const object of `type`.
Expected<void, Error> readText(std::string_view text, const DocumentFormat& format, const TypeInfo& type, void* target,
                               const TextReadOptions& options = {});

// Reads the envelope of a document of `format` (ADR-0024 rule 2) and returns its "data" tree,
// migrated to the current version: for documents whose content is keyed by names rather than a
// reflected type, such as configuration. The envelope checks, the options, the failures and the
// warnings for unknown document keys are those of readText.
Expected<Value, Error> readDocument(std::string_view text, const DocumentFormat& format,
                                    const TextReadOptions& options = {});

// Writes `data` as the "data" of a document of `format` at its current version, with the "id" of
// an asset document when `id` is given. Fails with "serialization.invalid_format",
// "serialization.invalid_id" (a nil id) and the writeJson errors.
Expected<std::string, Error> writeDocument(const DocumentFormat& format, const Value& data, const Uuid* id = nullptr);

// Options of fromValue: those of TextReadOptions that apply to a value without a document.
struct ValueReadOptions {
    UnknownFields unknownFields = UnknownFields::Warn;
    // Receives the warnings of a successful read, appended, as for readText. Null to not collect.
    std::vector<Error>* warnings = nullptr;
    // The bytes the read may allocate; 0 means kMinAllocationBudget (Document.h).
    std::size_t allocationBudget = 0;
    // How Entity values are read (Document.h); null: a saved Entity fails as for readText.
    const EntityMapping* entities = nullptr;
};

// The value of `type` at `object` as a Value tree, written as writeText writes the "data" of a
// document but without the document around it (ADR-0033): for values kept inside other
// documents, such as prefab overrides, and for editor command arguments. Error paths start at
// "$", the value itself. Fails as writeText does, except for the format and id errors.
Expected<Value, Error> toValue(const TypeInfo& type, const void* object, const EntityMapping* entities = nullptr);

// Reads a Value tree, as toValue writes it, into the value of `type` at `target`, as readText reads
// the "data" of a document. The whole value is built first, so on failure the target is
// unchanged. Fails as readText does, except for the document, format, version and migration
// errors. Precondition: `target` is a complete, non-const object of `type`.
Expected<void, Error> fromValue(const Value& value, const TypeInfo& type, void* target,
                                const ValueReadOptions& options = {});

// writeText for a reflected type T.
template <typename T>
Expected<std::string, Error> writeText(const DocumentFormat& format, const T& value, const Uuid* id = nullptr,
                                       const EntityMapping* entities = nullptr) {
    return writeText(format, typeOf<T>(), &value, id, entities);
}

// readText for a reflected type T.
template <typename T>
Expected<void, Error> readText(std::string_view text, const DocumentFormat& format, T& target,
                               const TextReadOptions& options = {}) {
    return readText(text, format, typeOf<T>(), &target, options);
}

} // namespace ith::serialization
