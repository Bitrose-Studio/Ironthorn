#pragma once

// The sidecar of an imported source file: `<file>.meta` (ADR-0015 rules 2, 3, 9 and 10).
//
// A sidecar holds the asset's id, the importer that reads the file and its version, the import
// settings, the sub-asset table and the engine-side overrides. It is a JSON text document
// (ADR-0010) with the asset id in its header, and it moves, copies and is committed with its file:
//
//   {
//     "format": "ith.asset_meta",
//     "version": 1,
//     "id": "3f2b8c1e-9d4a-4c1b-8e2f-0a1b2c3d4e5f",
//     "data": {
//       "importer": "gltf",
//       "importerVersion": 3,
//       "settings": {},
//       "subAssets": [
//         {"key": "mesh:Body/Hull", "id": "...", "missing": false},
//         {"key": "mesh:#0", "id": "...", "missing": true}
//       ],
//       "overrides": {}
//     }
//   }
//
// Sub-assets are keyed by a stable **source key**, `<type>:<name path>` from the source file, or
// `<type>:#<index>` for an unnamed object (see Importer.h). Reimport matches by key: matches keep
// their ids, new objects get new ids, and objects that disappeared stay marked missing, so
// references to them can be reported instead of silently breaking (rule 9). The table is kept
// sorted by key so that files diff cleanly. Overrides are stored and written back unchanged; they
// are applied by a later slice (rule 10).
//
// All functions are thread-safe for distinct arguments.

#include "ith/assets/AssetId.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Error.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

// The sidecar document format. Version 1 is the first.
inline constexpr serialization::DocumentFormat kSidecarFormat{
    .name = "ith.asset_meta", .version = 1, .minimumVersion = 1};

// The file name suffix of a sidecar.
inline constexpr std::string_view kSidecarSuffix = ".meta";

// The largest sidecar read, in bytes.
inline constexpr std::size_t kMaxSidecarSize = std::size_t{64} << 20;

// One row of the sub-asset table.
struct SubAssetEntry {
    // The stable source key, e.g. "mesh:Body/Hull" (isValidSourceKey).
    std::string key;
    // The sub-asset's id; never nil.
    SubAssetId id;
    // True when the object has disappeared from the source file since the id was given.
    bool missing = false;

    friend bool operator==(const SubAssetEntry&, const SubAssetEntry&) = default;
};

// The contents of a sidecar.
struct Sidecar {
    // The asset's id; never nil in a valid sidecar.
    AssetId id;
    // The id of the importer that reads the file, e.g. "gltf".
    std::string importer;
    // The version of that importer at the last import.
    std::uint32_t importerVersion = 0;
    // The import settings: a JSON object whose keys the importer defines.
    serialization::Value settings = serialization::Value::object();
    // The sub-asset table, sorted by key, with unique keys and unique ids.
    std::vector<SubAssetEntry> subAssets;
    // Engine-side overrides, keyed by sub-asset id and property path (ADR-0015 rule 10): a JSON
    // object, kept as it is until overrides are applied.
    serialization::Value overrides = serialization::Value::object();

    friend bool operator==(const Sidecar&, const Sidecar&) = default;
};

// True when `key` is a valid source key: a type of lowercase ASCII letters, digits and '_', a
// ':', then a non-empty rest.
[[nodiscard]] bool isValidSourceKey(std::string_view key) noexcept;

// Writes `sidecar` as a sidecar document of the current version. Fails with
// "asset.sidecar_invalid" when the sidecar breaks a rule above (nil id, empty importer,
// settings or overrides not an object, an invalid, unsorted or duplicate key, a nil or duplicate
// sub-asset id), or with the errors of serialization::writeJson.
[[nodiscard]] Expected<std::string, serialization::Error> writeSidecar(const Sidecar& sidecar);

// Reads a sidecar document. `source` names the file in errors. Older versions are migrated.
// Fails with the errors of serialization::parseJson (the limit is kMaxSidecarSize), with
// "serialization.not_a_document", "serialization.wrong_format", "serialization.newer_version" or
// "serialization.version_too_old" for the header, or with "asset.sidecar_invalid" for a missing
// or malformed field (the error's path names it) or a broken rule as for writeSidecar, except
// that an unsorted table is accepted and sorted. Unknown keys are dropped; when `warnings` is
// set, each appends a "serialization.unknown_field" warning.
[[nodiscard]] Expected<Sidecar, serialization::Error>
readSidecar(std::string_view text, std::string_view source = {}, std::vector<serialization::Error>* warnings = nullptr);

// Rule 9: the sub-asset table after an import that produced the objects with `keys` (unique).
// Existing rows whose key is in `keys` keep their ids and are no longer missing; keys without a
// row get new random ids; rows whose key is not in `keys` stay, marked missing. The result is
// sorted by key.
[[nodiscard]] std::vector<SubAssetEntry> reconcileSubAssets(std::span<const SubAssetEntry> table,
                                                            std::span<const std::string> keys);

} // namespace ith
