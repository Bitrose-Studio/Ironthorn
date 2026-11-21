#pragma once

// The header of an engine-native asset (ADR-0015 rule 2, ADR-0024).
//
// Engine-native assets (levels, prefabs, materials, data assets) are JSON text documents that
// hold their own id, so they need no sidecar:
//
//   {
//     "format": "ith.material",
//     "version": 2,
//     "id": "3f2b8c1e-9d4a-4c1b-8e2f-0a1b2c3d4e5f",
//     "data": { ... }
//   }
//
// Levels are ".ithmap" files and every other native asset an ".ithasset" file, both matched
// without ASCII case. The asset database reads only the header, without knowing the format, to
// learn the asset's id and type.
//
// All functions are thread-safe.

#include "ith/assets/AssetId.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Error.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ith {

// The file extension of native assets other than levels.
inline constexpr std::string_view kNativeAssetExtension = ".ithasset";

// The file extension of levels.
inline constexpr std::string_view kLevelExtension = ".ithmap";

// The document format of levels: the only format an ".ithmap" file holds, and one an ".ithasset"
// file never holds.
inline constexpr std::string_view kLevelFormat = "ith.level";

// True when `path` (UTF-8, '/'-separated) has a native asset extension, in any ASCII case.
[[nodiscard]] bool isNativeAssetPath(std::string_view path);

// True when a native asset at `path` may hold a document of `format`: a level file holds the
// level format, any other native asset file holds another format.
[[nodiscard]] bool formatMatchesExtension(std::string_view path, std::string_view format);

// The header of a native asset.
struct AssetHeader {
    // The document format, which is the asset's type, e.g. "ith.material".
    std::string format;
    // The document version.
    std::uint32_t version = 0;
    // The asset's id; never nil.
    AssetId id;

    friend bool operator==(const AssetHeader&, const AssetHeader&) = default;
};

// Reads the header of a native asset document. `source` names the file in errors. Fails with
// the errors of serialization::parseJson, with "serialization.not_a_document" when the document
// isn't an object with a string "format", a version number, a "data" member and an "id",
// "serialization.duplicate_field" when one of these four keys is given twice, or
// "asset.invalid_id" when the id is not a UUID or is nil. Doesn't read "data".
[[nodiscard]] Expected<AssetHeader, serialization::Error> readAssetHeader(std::string_view text,
                                                                          std::string_view source = {});

// Returns `text`, a native asset or sidecar document, with its header id replaced by `id`, for
// giving a copied asset a new id. Fails as readAssetHeader does (without checking the old id), or
// with the errors of serialization::writeJson. The output is written as writeJson writes it.
[[nodiscard]] Expected<std::string, serialization::Error> replaceHeaderId(std::string_view text, AssetId id,
                                                                          std::string_view source = {});

} // namespace ith
