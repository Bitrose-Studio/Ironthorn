#pragma once

// The annotations that mark types and fields for reflection (ADR-0018, ADR-0019).
//
//   #include "ith/reflection/Annotations.h"
//
//   namespace game {
//   struct Health {
//       ITH_TYPE(Component, Category = "Combat")
//       ITH_FIELD(Min = 0, Max = 10000, Tooltip = "Hit points", Replicated, SaveGame)
//       float current = 100.0f;
//       ITH_FIELD(Min = 1, Max = 10000)
//       float maximum = 100.0f;
//       float cachedRatio = 1.0f; // not reflected
//   };
//
//   enum class DamageKind : std::uint8_t { Physical, Fire, Poison };
//   ITH_ENUM(DamageKind, Tooltip = "How damage is dealt")
//   } // namespace game
//
// The ith_reflect generator reads these annotations from the module's headers and writes the
// records; docs/development/reflection.md lists the attributes and the syntax rules. None of
// them changes a type's layout, and none is followed by a semicolon.
// - ITH_TYPE declares the static function the generated file defines, a declaration-only marker
//   that tells a reflected type from a type derived from it, and befriends the reflection
//   runtime so that private fields can be reflected.
// - ITH_FIELD expands to nothing.
// - ITH_ENUM declares, in the enum's namespace, the function the generated file defines; typeOf
//   finds it by argument-dependent lookup.

namespace ith {

struct TypeInfo;

namespace detail {
struct ReflectionAccess;
} // namespace detail
} // namespace ith

// Marks a struct or class for reflection. The first line of its body. Arguments: a role
// (Struct, Component, Resource, Event, Asset), then attributes.
#define ITH_TYPE(...)                                                                                                  \
    friend struct ::ith::detail::ReflectionAccess;                                                                     \
    static const ::ith::TypeInfo& ithTypeInfo() noexcept;                                                              \
    void ithReflected() const noexcept;

// Marks the data member declared on the following line for reflection. Arguments: attributes.
#define ITH_FIELD(...)

// Marks the enum class declared directly before it for reflection. Arguments: the enum's name,
// then attributes; `Flags` marks an enum whose values combine as bit flags.
#define ITH_ENUM(name, ...) const ::ith::TypeInfo& ithEnumTypeInfo(name*) noexcept;
