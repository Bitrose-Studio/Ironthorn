#include "ith/reflection/Entities.h"

#include "ith/foundation/Assert.h"

#include <cstddef>

namespace ith {
namespace {

void remap(const TypeInfo& type, void* value, const EntityMap& map) {
    if (!type.containsEntities) {
        return;
    }
    switch (type.kind) {
        case TypeKind::Entity: {
            auto* entity = static_cast<Entity*>(value);
            *entity = map.function(map.context, *entity);
            return;
        }
        case TypeKind::Struct:
            for (const FieldInfo& field : type.fields) {
                remap(field.type(), static_cast<std::byte*>(value) + field.offset, map);
            }
            return;
        case TypeKind::FixedArray:
        case TypeKind::DynamicArray:
        case TypeKind::Optional: {
            const TypeInfo& element = type.element();
            if (!element.containsEntities) {
                return;
            }
            const std::size_t count = type.container->size(value);
            for (std::size_t index = 0; index < count; ++index) {
                remap(element, type.container->at(value, index), map);
            }
            return;
        }
        case TypeKind::Bool:
        case TypeKind::SignedInteger:
        case TypeKind::UnsignedInteger:
        case TypeKind::Float:
        case TypeKind::String:
        case TypeKind::Uuid:
        case TypeKind::Enum:
            return; // leaves and enums hold no entity
    }
}

} // namespace

void remapEntities(const TypeInfo& type, void* value, const EntityMap& map) {
    const bool hasFunction = map.function != nullptr;
    ITH_ASSERT_MSG(hasFunction, "remapEntities: the EntityMap has no function");
    if (hasFunction) {
        remap(type, value, map);
    }
}

} // namespace ith
