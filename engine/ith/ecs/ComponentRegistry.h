#pragma once

// The process-wide table of component types (ADR-0014 rule 5). Internal to the ecs module:
// gameplay code names components by type (World::add<T>) and, from slice 2 on, by reflection
// type id; it never sees these indices.
//
// A component type is a reflected struct with the Component role (ITH_TYPE(Component)). Its
// identity is reflection's stable type id; the first time a type is used, the table gives it a
// dense index that every World shares, and keeps its TypeInfo, whose move and destroy
// functions the archetype tables call (or memcpy, for trivially copyable types). Dense indices
// are process-local and depend on the order of first use: never store them.
//
// Thread safety: registering takes a mutex; reading a registered type does not. Every function
// here may be called from any thread.

#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace ith::detail {

// The dense index of a component type.
using ComponentIndex = std::uint32_t;

// How many component types a process can have. Archetypes keep a bitset of this size, so
// raising it costs memory per archetype, not per entity.
inline constexpr std::size_t kMaxComponentTypes = 4096;

// The index returned for a type that is not a component. No archetype has it: lookups with it
// find nothing, and spawn and add with it are refused.
inline constexpr ComponentIndex kNoComponent = std::numeric_limits<ComponentIndex>::max();

// The index of `type`, registering it on first use; a type with the same id always gets the
// same index, even through another copy of its record.
// - A type that is not a reflected struct with the Component role asserts and gets
//   kNoComponent; it is not registered.
// - A record whose id is registered with another name, size or alignment is fatal: two types
//   share an id, and storing one as the other would corrupt memory.
// - Exceeding kMaxComponentTypes is fatal.
// The record must outlive the process (generated records do).
ComponentIndex registerComponentType(const TypeInfo& type);

// The record of a registered index. Precondition: `index` was returned by
// registerComponentType and is not kNoComponent; anything else is fatal.
[[nodiscard]] const TypeInfo& componentType(ComponentIndex index) noexcept;

// The index of component type T. After the first call per type this is a single load.
template <typename T>
ComponentIndex componentIndexOf() {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>,
                  "a component type is a plain type, not const or a reference");
    static const ComponentIndex kIndex = registerComponentType(typeOf<T>());
    return kIndex;
}

} // namespace ith::detail
