#pragma once

// Document formats, versions, migrations and the limits shared by every reader (ADR-0010,
// ADR-0020).
//
// Every saved document names its format and version: a text document in its "format" and
// "version" fields, a binary blob in its header. Writers always write the current version.
//
//   // Version 1 called the field "hp"; version 2 calls it "current".
//   ith::Expected<void, ith::serialization::Error> renameHp(ith::serialization::Value& data) {
//       if (ith::serialization::Value* hp = data.find("hp")) {
//           ith::serialization::Value moved = std::move(*hp);
//           data.remove("hp");
//           data.set("current", std::move(moved));
//       }
//       return {};
//   }
//   constexpr ith::serialization::Migration kHealthMigrations[] = {&renameHp};
//   constexpr ith::serialization::DocumentFormat kHealthFormat{
//       .name = "game.health", .version = 2, .minimumVersion = 1, .migrations = kHealthMigrations};
//
// Text documents older than the current version are migrated step by step, version N to N + 1,
// before reflection reads them; the chain runs all-or-nothing on a copy, so a failed migration
// leaves the target untouched. Binary (cooked) data is never migrated: a blob of another version
// is refused and must be recooked.

#include "ith/foundation/Expected.h"
#include "ith/serialization/Error.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace ith::serialization {

// The deepest nesting a reader accepts and a writer produces. In JSON every array and object
// counts, the document's own object included; in binary data every struct and array counts.
inline constexpr std::size_t kMaxDepth = 64;

// The largest text document, in bytes, that is read or written. Readers may set a lower limit.
inline constexpr std::size_t kMaxTextSize = std::size_t{256} << 20;

// Reads of untrusted data allocate at most an allocation budget: by default the larger of
// kMinAllocationBudget and kAllocationBudgetFactor times the input size. A read that would need
// more fails with "serialization.too_large" before it allocates, so a small input can't make a
// reader allocate gigabytes.
inline constexpr std::size_t kMinAllocationBudget = std::size_t{64} << 20;
inline constexpr std::size_t kAllocationBudgetFactor = 16;

// The allocation budget of a read of `inputSize` bytes: `requested` when it isn't 0, the default
// above otherwise.
constexpr std::size_t allocationBudget(std::size_t inputSize, std::size_t requested) noexcept {
    if (requested != 0) {
        return requested;
    }
    const std::size_t scaled = inputSize > std::numeric_limits<std::size_t>::max() / kAllocationBudgetFactor
                                   ? std::numeric_limits<std::size_t>::max()
                                   : inputSize * kAllocationBudgetFactor;
    return scaled > kMinAllocationBudget ? scaled : kMinAllocationBudget;
}

// Moves the data of a text document (the value of its "data" field) from one version to the
// next, in place. A failure stops the load; its code, message and path are reported inside a
// "serialization.migration_failed" error.
// - Must not keep pointers or references into `data`.
// - To put a part of `data` somewhere else in `data`, move it into a local Value first, then
//   assign it: assigning a subtree straight into its own parent destroys it while it is read.
// - The result must stay within kMaxDepth.
using Migration = Expected<void, Error> (*)(Value& data);

// The name and version of a document format and the migrations of its text form.
struct DocumentFormat {
    // Stable identifier written into every document, e.g. "game.health". 1 to 255 bytes.
    std::string_view name;
    // The current version, which writers write. At least 1.
    std::uint32_t version = 1;
    // The oldest text version that is still migrated. Older documents are refused with an error
    // that names this version. At most `version`; a format without migrations sets it to
    // `version`.
    std::uint32_t minimumVersion = 1;
    // migrations[i] moves version minimumVersion + i to minimumVersion + i + 1, so there are
    // exactly version - minimumVersion of them.
    std::span<const Migration> migrations;
};

} // namespace ith::serialization
