#pragma once

// TypeRegistry: finds reflected types by id or name (ADR-0018).
//
//   ITH_DECLARE_REFLECTION_MODULE(gameplay) // the function ith_reflect generates for a module
//
//   ith::TypeRegistry registry;                         // holds the leaf types already
//   ith::generated::registerReflection_gameplay(registry); // at startup, once per module
//   const ith::TypeInfo* health = registry.find("game::Health");
//
// The registry holds pointers to the static records of the generated files and of the leaf
// types; it owns no records. Each module's generated registration function adds its types;
// applications call these functions explicitly at startup (ADR-0017 rule 5), since static
// initializers are dropped from static libraries. A FormerName registers the old name and id
// as aliases, so data written under the old name still finds the type.
// A module's entries must be removed before the module is unloaded; the hot reload ADR defines
// how (ADR-0019).
//
// Thread safety: filling the registry is not thread-safe and happens at startup, before other
// threads read it. After that it is read-only and every const function is thread-safe.
// Lookups don't allocate.

#include "ith/foundation/Expected.h"
#include "ith/reflection/TypeInfo.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ith {

class TypeRegistry {
public:
    // A registry that holds the leaf types (bool, the integers, float, double, std::string,
    // ith::Uuid).
    TypeRegistry();

    // Adds a named type: a leaf, an ITH_TYPE struct or an ITH_ENUM enum. The record must have
    // static storage duration (a generated or leaf record) or otherwise outlive the registry.
    // Fails with "reflection.duplicate_id" when its id or the id of its FormerName is already
    // taken, by another type or by this one; the message names both types. Precondition (asserted): the type is not a container and its id is
    // fnv1a64(name).
    Expected<void> add(const TypeInfo& type);

    // Adds the types of one module, as add() does. A collision is a fatal error naming both
    // types: two types can't share an id in one process. Called by generated registration
    // functions.
    void addAll(std::span<const TypeInfo* const> types);

    // The type with this id, or with this FormerName id; null when there is none.
    [[nodiscard]] const TypeInfo* find(std::uint64_t id) const noexcept;

    // The type with this fully qualified name or FormerName; null when there is none.
    [[nodiscard]] const TypeInfo* find(std::string_view name) const noexcept;

    // Every registered type in registration order, leaf types first.
    [[nodiscard]] std::span<const TypeInfo* const> types() const noexcept { return m_types; }

private:
    std::vector<const TypeInfo*> m_types;
    std::unordered_map<std::uint64_t, const TypeInfo*> m_byId; // names and FormerNames
};

} // namespace ith

// Declares the registration function ith_reflect generates for `module`:
//   void ith::generated::registerReflection_<module>(ith::TypeRegistry& registry);
// Use it at namespace scope (outside any namespace).
#define ITH_DECLARE_REFLECTION_MODULE(module)                                                                          \
    namespace ith::generated {                                                                                         \
    void registerReflection_##module(::ith::TypeRegistry& registry);                                                   \
    }
