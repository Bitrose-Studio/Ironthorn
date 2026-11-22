#pragma once

// ith::UnresolvedComponents: the components of a level entity that loading couldn't read, kept so
// that saving writes them back (ADR-0034 rule 13).
//
// A component whose type name the registry doesn't know (a disabled plugin, a rename without
// FormerName), whose name isn't a saved component type, or whose value fromValue refuses, is kept
// here as the raw JSON value it had in the level file. saveLevel writes each entry back under its
// type name, in the section it came from, so a level never loses a component because its type is
// missing for a while. Once the type is known again and its value reads, the next load reads it
// into a real component.
//
// The component is Transient: it is never saved as itself, and a cooked level can't contain one
// (an unresolved component is a cook error). Copies of a world (Play In Editor) keep it.

#include "ith/reflection/Annotations.h"
#include "ith/serialization/Value.h"

#include <string>
#include <vector>

namespace ith {

// One component that loading couldn't read.
struct UnresolvedComponent {
    // The key it had in the level file: a fully qualified type name, as written.
    std::string typeName;
    // True when it came from the record's "editor" section, false for "components".
    bool editor = false;
    // Its value as the file held it.
    serialization::Value value;

    friend bool operator==(const UnresolvedComponent&, const UnresolvedComponent&) = default;
};

// The unresolved components of one entity, in the order the file listed them. Set by loadLevel;
// code that knows a component's type should add the component itself, which saveLevel then
// writes instead of an entry with the same type name.
struct UnresolvedComponents {
    ITH_TYPE(Component, Transient)
    // Not reflected: a raw value has no reflected type.
    std::vector<UnresolvedComponent> entries;
};

} // namespace ith
