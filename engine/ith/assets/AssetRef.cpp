#include "ith/assets/AssetRef.h"

#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

namespace ith {

AssetReferenceKind assetReferenceKind(const TypeInfo& type) noexcept {
    if (type.id == typeOf<AssetRef>().id) {
        return AssetReferenceKind::Hard;
    }
    if (type.id == typeOf<SoftAssetRef>().id) {
        return AssetReferenceKind::Soft;
    }
    return AssetReferenceKind::None;
}

} // namespace ith
