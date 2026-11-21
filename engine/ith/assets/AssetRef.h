#pragma once

// References between assets (ADR-0015 rule 4).
//
// A reference names an asset by id, plus a sub-asset id where it points inside one, and never by
// path: files store the ids only, so a rename or move can't make a reference stale. The editor
// shows names. Whether a reference is hard or soft is part of the reflected property, given by
// its type:
//
// - AssetRef (hard): loaded together with the asset that holds it, e.g. a mesh's material;
// - SoftAssetRef (soft): loaded on request, asynchronously, e.g. the next level.
//
//   struct Prop {
//       ITH_TYPE(Component)
//       ITH_FIELD()
//       ith::AssetRef mesh;      // {"asset": "<uuid>", "subAsset": "<uuid>"}
//       ITH_FIELD()
//       ith::SoftAssetRef next;
//   };
//   prop.mesh = ith::AssetRef::to(meshFile, bodyMesh);
//
// A nil asset id is "no reference"; a nil sub-asset id means the asset itself. Both are plain
// values: all functions are thread-safe.

#include "ith/assets/AssetId.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/Annotations.h"

#include <cstdint>

namespace ith {

struct TypeInfo;

// A hard reference: the referenced asset loads with the asset that holds the reference.
struct AssetRef {
    ITH_TYPE(Struct)
    // The referenced asset; nil for no reference.
    ITH_FIELD()
    Uuid asset;
    // The sub-asset inside it; nil for the asset itself.
    ITH_FIELD()
    Uuid subAsset;

    // A reference to `id`, or to its sub-asset `subId`.
    [[nodiscard]] static AssetRef to(AssetId id, SubAssetId subId = {}) noexcept {
        return AssetRef{.asset = id.uuid(), .subAsset = subId.uuid()};
    }

    [[nodiscard]] AssetId assetId() const noexcept { return AssetId(asset); }

    [[nodiscard]] SubAssetId subAssetId() const noexcept { return SubAssetId(subAsset); }

    // True when the reference names an asset.
    [[nodiscard]] bool isSet() const noexcept { return !asset.isNil(); }

    friend bool operator==(const AssetRef&, const AssetRef&) = default;
};

// A soft reference: the referenced asset loads on request. Same contents as AssetRef.
struct SoftAssetRef {
    ITH_TYPE(Struct)
    // The referenced asset; nil for no reference.
    ITH_FIELD()
    Uuid asset;
    // The sub-asset inside it; nil for the asset itself.
    ITH_FIELD()
    Uuid subAsset;

    // A reference to `id`, or to its sub-asset `subId`.
    [[nodiscard]] static SoftAssetRef to(AssetId id, SubAssetId subId = {}) noexcept {
        return SoftAssetRef{.asset = id.uuid(), .subAsset = subId.uuid()};
    }

    [[nodiscard]] AssetId assetId() const noexcept { return AssetId(asset); }

    [[nodiscard]] SubAssetId subAssetId() const noexcept { return SubAssetId(subAsset); }

    // True when the reference names an asset.
    [[nodiscard]] bool isSet() const noexcept { return !asset.isNil(); }

    friend bool operator==(const SoftAssetRef&, const SoftAssetRef&) = default;
};

// How a reflected type refers to assets.
enum class AssetReferenceKind : std::uint8_t {
    None, // not a reference type
    Hard, // AssetRef
    Soft, // SoftAssetRef
};

// The reference kind of `type`, compared by type id (ADR-0019): Hard for AssetRef, Soft for
// SoftAssetRef, None for every other type, containers of references included (walk their element
// type). Used by tools that follow references, such as the cooker and the dependents index.
[[nodiscard]] AssetReferenceKind assetReferenceKind(const TypeInfo& type) noexcept;

} // namespace ith
