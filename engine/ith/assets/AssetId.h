#pragma once

// Asset and sub-asset identity (ADR-0015 rules 1 and 3).
//
// Every asset has a random AssetId, created when the asset is created or first imported and never
// derived from its path, so references survive renames and moves. A sub-asset (a mesh, material
// or animation inside one imported file) has its own random SubAssetId. The two are distinct
// types so that one can't be passed where the other is expected.
//
//   const ith::AssetId id = ith::AssetId::generate();
//   const std::string text = id.toString();                // the Uuid's canonical form
//   const ith::Expected<ith::AssetId> parsed = ith::AssetId::parse(text);
//   std::unordered_map<ith::AssetId, Record> byId;          // hashable
//
// All functions are thread-safe.

#include "ith/foundation/Expected.h"
#include "ith/foundation/Uuid.h"

#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace ith {

// A Uuid with a tag that tells what kind of object it names. Use the aliases AssetId and
// SubAssetId below.
template <typename Tag>
class TypedId {
public:
    // The nil id, which means "nothing".
    constexpr TypedId() noexcept = default;

    // An id holding `uuid` as it is.
    constexpr explicit TypedId(Uuid uuid) noexcept : m_uuid(uuid) {}

    // A new random id (Uuid::generate()).
    [[nodiscard]] static TypedId generate() { return TypedId(Uuid::generate()); }

    // Parses the canonical text form of a Uuid. Fails with "uuid.malformed".
    [[nodiscard]] static Expected<TypedId> parse(std::string_view text) {
        Expected<Uuid> uuid = Uuid::parse(text);
        if (!uuid) {
            return makeUnexpected(std::move(uuid).error());
        }
        return TypedId(*uuid);
    }

    // The canonical lowercase text form (36 characters).
    [[nodiscard]] std::string toString() const { return m_uuid.toString(); }

    // The underlying Uuid, as stored in files.
    [[nodiscard]] constexpr Uuid uuid() const noexcept { return m_uuid; }

    // True for the nil id.
    [[nodiscard]] constexpr bool isNil() const noexcept { return m_uuid.isNil(); }

    constexpr auto operator<=>(const TypedId&) const noexcept = default;

private:
    Uuid m_uuid;
};

// The identity of an asset: a file in the project's Content folder.
using AssetId = TypedId<struct AssetIdTag>;

// The identity of a sub-asset inside an asset, unique within that asset.
using SubAssetId = TypedId<struct SubAssetIdTag>;

} // namespace ith

// Lets AssetId and SubAssetId be keys of std::unordered_map and std::unordered_set.
template <typename Tag>
struct std::hash<ith::TypedId<Tag>> {
    std::size_t operator()(const ith::TypedId<Tag>& id) const noexcept { return std::hash<ith::Uuid>()(id.uuid()); }
};
