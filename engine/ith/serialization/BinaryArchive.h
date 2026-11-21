#pragma once

// The binary archive: any reflected value as a compact little-endian blob (ADR-0010, ADR-0020). Cooked
// data is saved this way.
//
//   constexpr ith::serialization::DocumentFormat kLoadoutFormat{.name = "game.loadout", .version = 1};
//   const auto blob = ith::serialization::writeBinary(kLoadoutFormat, loadout);
//   const auto read = ith::serialization::readBinary(*blob, kLoadoutFormat, loadout, {.source = "loadout.bin"});
//
// A blob is a header followed by the payload:
//
//   offset  size  content
//   0       4     magic "ITHB"
//   4       4     format version (uint32)
//   8       8     schema hash of the root type (uint64, schemaHash below)
//   16      8     payload size in bytes (uint64)
//   24      2     format name length n, 1 to 255 (uint16)
//   26      n     format name (UTF-8)
//   26 + n        zero bytes up to the next multiple of 16, where the payload starts
//
// The payload is the value, with no padding and no field names. Numbers are little-endian at
// their own width, floats are their IEEE bits (so every value, NaN payloads included, reads
// back bit-identical), bool and the presence byte of an optional are 0 or 1, std::string and
// std::vector start with a uint32 count (strings are bytes, not checked
// for UTF-8), ith::Uuid is its high then low 64 bits, an enum is its
// underlying integer, a struct is its saved fields in declaration order, and a std::array is
// its elements. Transient and EditorOnly fields are left out: cooked data has no editor data
// (ADR-0017). A std::vector whose elements encode to zero bytes is refused.
//
// Cooked data isn't migrated: a blob of another format version or schema hash is refused and
// must be recooked. The reader never trusts the data: every count and size is checked against
// the bytes left and against the allocation budget (Document.h) before anything is allocated,
// booleans and enums are validated, nesting is limited to kMaxDepth, and the payload must be
// consumed exactly. A failed read leaves the target unchanged; a successful one replaces the
// whole object, so Transient, EditorOnly and unreflected members get their default values.
//
// All functions are thread-safe for distinct targets.

#include "ith/foundation/Expected.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ith::serialization {

struct BinaryReadOptions {
    // The file name or other source, copied into errors.
    std::string_view source;
    // The bytes the read may allocate; 0 means the default of allocationBudget (Document.h).
    std::size_t allocationBudget = 0;
};

// A hash of everything the binary encoding of `type` depends on: the ids, kinds and encoded
// widths (not C++ object sizes) of the type and of every type it contains, the ids of its saved fields and the names and values
// of its enumerators. Any change to these changes the hash, so a blob is never read with a
// layout it wasn't written with. Stable across compilers, platforms and configurations.
[[nodiscard]] std::uint64_t schemaHash(const TypeInfo& type);

// Writes the value of `type` at `object` as a blob of `format` at its current version. Fails
// with "serialization.not_saved" (the type, or an element type, is Transient or EditorOnly),
// "serialization.invalid_enum", "serialization.unsupported_type" (a vector of zero-byte
// elements), "serialization.too_deep" or "serialization.too_large" (a string or vector of more
// than 2^32 - 1 elements). Precondition (asserted): `format` is valid (Document.h).
Expected<std::vector<std::byte>, Error> writeBinary(const DocumentFormat& format, const TypeInfo& type,
                                                    const void* object);

// Reads a blob of `format` into the value of `type` at `target`. On failure the target is
// unchanged. Fails with:
// - "serialization.bad_magic", "serialization.truncated", "serialization.trailing_data";
// - "serialization.wrong_format", "serialization.newer_version", "serialization.version_too_old"
//   (cooked data of an older version must be recooked), "serialization.schema_mismatch";
// - "serialization.invalid_value" (a bool or presence byte that isn't 0 or 1, an enum value
//   that isn't valid, a nonzero padding byte), "serialization.too_deep",
//   "serialization.not_saved", "serialization.unsupported_type";
// - "serialization.too_large": the value needs more than the allocation budget.
// Errors carry the byte offset and, inside the payload, the path. Preconditions: `format` is
// valid (asserted); `target` is a complete, non-const object of `type`.
Expected<void, Error> readBinary(std::span<const std::byte> data, const DocumentFormat& format, const TypeInfo& type,
                                 void* target, const BinaryReadOptions& options = {});

// writeBinary for a reflected type T.
template <typename T>
Expected<std::vector<std::byte>, Error> writeBinary(const DocumentFormat& format, const T& value) {
    return writeBinary(format, typeOf<T>(), &value);
}

// readBinary for a reflected type T.
template <typename T>
Expected<void, Error> readBinary(std::span<const std::byte> data, const DocumentFormat& format, T& target,
                                 const BinaryReadOptions& options = {}) {
    return readBinary(data, format, typeOf<T>(), &target, options);
}

} // namespace ith::serialization
