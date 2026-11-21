#pragma once

// The cached output of one import: the derived-data cache entry the asset database writes for a
// source file (ADR-0015 rule 11), and that the cooker will read.

#include "ith/reflection/Annotations.h"
#include "ith/serialization/Document.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ith {

// One imported object with its source key.
struct ImportedObjectData {
    ITH_TYPE(Struct)
    // The stable source key (Importer.h, sourceKeys).
    ITH_FIELD()
    std::string key;
    // The importer's output for the object.
    ITH_FIELD()
    std::vector<std::uint8_t> payload;
};

// Every object of one source file, in source order.
struct ImportedAssetData {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::vector<ImportedObjectData> objects;
};

// The binary format of ImportedAssetData entries.
inline constexpr serialization::DocumentFormat kImportedAssetFormat{.name = "ith.imported_asset", .version = 1};

} // namespace ith
