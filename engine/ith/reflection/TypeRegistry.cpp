#include "ith/reflection/TypeRegistry.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Hash.h"
#include "ith/reflection/TypeOf.h"

#include <array>
#include <charconv>
#include <initializer_list>
#include <string>
#include <utility>

namespace ith {
namespace {

std::string hexId(std::uint64_t id) {
    std::array<char, 16> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), id, 16);
    return "0x" + std::string(digits.data(), result.ptr);
}

[[maybe_unused]] bool isContainer(TypeKind kind) {
    return kind == TypeKind::FixedArray || kind == TypeKind::DynamicArray || kind == TypeKind::Optional;
}

} // namespace

TypeRegistry::TypeRegistry() {
    addAll(detail::kLeafTypes);
    addAll(detail::kMathTypes);
}

Expected<void> TypeRegistry::add(const TypeInfo& type) {
    ITH_ASSERT_MSG(!isContainer(type.kind), "TypeRegistry::add: containers have no name to register");
    ITH_ASSERT_MSG(type.id == fnv1a64(type.name), "TypeRegistry::add: the type id is not fnv1a64 of its name");

    const std::string_view formerName = type.attributes.formerName;
    const std::uint64_t aliasId = formerName.empty() ? type.id : fnv1a64(formerName);

    for (const auto& [id, name] : {std::pair{type.id, type.name}, std::pair{aliasId, formerName}}) {
        const auto existing = m_byId.find(id);
        if (existing != m_byId.end()) {
            return makeError("reflection.duplicate_id", "'" + std::string(type.name) + "' can't be registered: id " +
                                                            hexId(id) + " (of '" + std::string(name) +
                                                            "') is already taken by '" +
                                                            std::string(existing->second->name) + "'.");
        }
    }

    m_types.push_back(&type);
    m_byId.emplace(type.id, &type);
    m_byId.emplace(aliasId, &type);
    return {};
}

void TypeRegistry::addAll(std::span<const TypeInfo* const> types) {
    for (const TypeInfo* type : types) {
        ITH_ASSERT(type != nullptr);
        if (Expected<void> added = add(*type); !added) {
            ITH_FATAL(added.error().message.c_str());
        }
    }
}

const TypeInfo* TypeRegistry::find(std::uint64_t id) const noexcept {
    const auto found = m_byId.find(id);
    return found != m_byId.end() ? found->second : nullptr;
}

const TypeInfo* TypeRegistry::find(std::string_view name) const noexcept {
    const TypeInfo* type = find(fnv1a64(name));
    // A different name can share a hash with a registered one; only an exact match counts.
    if (type != nullptr && (type->name == name || type->attributes.formerName == name)) {
        return type;
    }
    return nullptr;
}

} // namespace ith
