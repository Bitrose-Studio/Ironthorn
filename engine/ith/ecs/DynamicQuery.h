#pragma once

// ith::DynamicQuery: iteration over the entities of a World by component type record (ADR-0014
// rule 7), for code that knows its component types only at run time: the editor, serialization
// and the visual-script VM.
//
//   const ith::TypeInfo* health = typeRegistry.find("game::Health");
//   const std::array terms{ith::QueryTerm{.type = health, .access = ith::Access::Write}};
//   ith::DynamicQuery query = world.dynamicQuery(terms);
//   query.forEach([&](ith::Entity entity, std::span<void* const> components) {
//       // components[i] points to the entity's value of terms[i].type
//   });
//
// A dynamic query visits every entity that has all of its component types, archetype by archetype
// and row by row, with the same matching code as the typed Query. A query without terms and
// filters visits every entity. Each term declares read or write access, as `const T` and `T` do
// for a typed query; inside a system it must match the system's declared access (Schedule.h), and
// change detection (slice 6) will use it. Code must not write through the pointer of a read term.
//
// Filters (QueryFilter) narrow the entities without handing out values, as ith::With and
// ith::Without do for a typed query: FilterKind::With requires the type and FilterKind::Without
// skips entities that have it. They are not in the `components` span and need no declared access
// inside a system.
//
//   const std::array filters{ith::QueryFilter{.type = frozen, .kind = ith::FilterKind::Without}};
//   ith::DynamicQuery moving = world.dynamicQuery(terms, filters);
//
// Invalid terms and filters are programmer errors: a null type, a type that is not a reflected
// struct with the Component role, a component type named twice (terms and filters together), or a
// write term of an immutable component type (ADR-0030) asserts, and the query then visits nothing.
//
// The query keeps its own cache of matching archetypes and extends it with the archetypes created
// since it last ran. No structural change may happen while it iterates (asserted and refused), as
// for the typed Query. A DynamicQuery refers to its World and is valid while the World lives; it
// can be kept and reused. Not thread-safe: use it on the thread that owns the World.

#include "ith/ecs/Archetype.h"
#include "ith/ecs/Entity.h"
#include "ith/ecs/Query.h"
#include "ith/reflection/TypeInfo.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace ith {

class World;

// How a query term or a system uses a component.
enum class Access : std::uint8_t {
    Read,
    Write,
};

// One component type of a dynamic query and how the query uses it.
struct QueryTerm {
    const TypeInfo* type = nullptr; // a reflected struct with the Component role
    Access access = Access::Read;
};

// What a query filter asks of an entity.
enum class FilterKind : std::uint8_t {
    With,    // it has the type
    Without, // it doesn't have the type
};

// A filter of a dynamic query: it narrows the entities without handing out a value.
struct QueryFilter {
    const TypeInfo* type = nullptr; // a reflected struct with the Component role
    FilterKind kind = FilterKind::With;
};

class DynamicQuery {
public:
    // Calls `function(Entity, std::span<void* const> components)` for every matching entity, where
    // components[i] is the entity's value of terms()[i]. The order is by archetype, then by row;
    // it is deterministic for one sequence of structural changes but is not otherwise specified.
    // Structural changes inside `function` are refused. Allocates one small array per call.
    template <typename Function>
    void forEach(Function&& function);

    // The terms, as given to World::dynamicQuery.
    [[nodiscard]] std::span<const QueryTerm> terms() const noexcept { return m_terms; }

private:
    friend class World;

    DynamicQuery(std::span<const QueryTerm> terms, std::vector<std::unique_ptr<detail::Archetype>>& archetypes,
                 std::uint32_t& iterationDepth);

    std::vector<QueryTerm> m_terms;
    detail::QueryState m_state;
    std::vector<std::unique_ptr<detail::Archetype>>* m_archetypes;
    std::uint32_t* m_iterationDepth;
};

template <typename Function>
void DynamicQuery::forEach(Function&& function) {
    static_assert(std::is_invocable_v<Function&, Entity, std::span<void* const>>,
                  "the function must take (ith::Entity, std::span<void* const>)");
    detail::updateQueryState(m_state, *m_archetypes);
    const detail::IterationScope scope(*m_iterationDepth);
    const std::size_t terms = m_state.terms.size();
    std::vector<void*> values(terms);
    const std::uint32_t* columns = m_state.columns.data();
    for (const detail::ArchetypeIndex index : m_state.archetypes) {
        detail::Archetype& archetype = *(*m_archetypes)[index];
        const std::span<const Entity> entities = archetype.entities();
        for (std::size_t row = 0; row < entities.size(); ++row) {
            for (std::size_t term = 0; term < terms; ++term) {
                values[term] = archetype.column(columns[term]).at(row);
            }
            function(entities[row], std::span<void* const>(values));
        }
        columns += terms;
    }
}

} // namespace ith
