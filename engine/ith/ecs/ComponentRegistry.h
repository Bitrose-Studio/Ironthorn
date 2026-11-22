#pragma once

// The process-wide table of component types (ADR-0014 rule 5). Internal to the ecs module:
// gameplay code names components by type (World::add<T>) or by reflection record
// (World::add(entity, type), whose id is the component id); it never sees these indices.
//
// A component type is a reflected struct with the Component role (ITH_TYPE(Component)). Its
// identity is reflection's stable type id; the first time a type is used, the table gives it a
// dense index that every World shares, and keeps its TypeInfo, whose move and destroy
// functions the archetype tables call (or memcpy, for trivially copyable types). Dense indices
// are process-local and depend on the order of first use: never store them.
//
// Thread safety: registering takes a mutex; reading a registered type does not. Every function
// here may be called from any thread.

#include "ith/foundation/Assert.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace ith {
struct ComponentHooks;
} // namespace ith

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

// Sets the hooks of a registered index (ith::setComponentHooks). Returns false, asserting, when
// the index already has hooks or was stored by a World.
bool setComponentHooks(ComponentIndex index, const ComponentHooks& hooks);

// Marks a registered index as stored by a World, after which its hooks can't be set, and returns
// its hooks, or null when it has none. The hooks never change afterwards, so the pointer can be
// kept and read without a lock. Archetypes call it when they are created.
[[nodiscard]] const ComponentHooks* useComponentType(ComponentIndex index);

// True when values of `type` may be written in place; asserts for an immutable component type
// (ADR-0030), which changes only by being replaced through add.
inline bool isWritableComponent(const TypeInfo& type) noexcept {
    const bool writable = !hasFlags(type.flags, ReflectionFlags::Immutable);
    ITH_ASSERT_MSG(writable, "ecs: an immutable component can't be written in place; replace it with add");
    return writable;
}

// True when T is an immutable component type. After the first call per type this is a single load.
template <typename T>
bool isImmutableComponent() {
    static const bool kImmutable = hasFlags(typeOf<T>().flags, ReflectionFlags::Immutable);
    return kImmutable;
}

// The index of component type T. After the first call per type this is a single load.
template <typename T>
ComponentIndex componentIndexOf() {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>,
                  "a component type is a plain type, not const or a reference");
    static const ComponentIndex kIndex = registerComponentType(typeOf<T>());
    return kIndex;
}

} // namespace ith::detail
