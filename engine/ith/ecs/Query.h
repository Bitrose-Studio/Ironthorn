#pragma once

// ith::Query: typed iteration over the entities of a World (ADR-0014 rule 7).
//
//   world.query<Position, const Velocity>().forEach([](Position& position, const Velocity& velocity) {
//       position.x += velocity.x;
//   });
//   world.query<const Health>().forEach([](ith::Entity entity, const Health& health) { ... });
//
// A query visits every entity that has all of its component types, archetype by archetype and
// row by row. A term written as `const T` reads T; a term written as plain `T` writes it. The
// schedule (slice 3) will use these declarations to order and parallelize systems, and change
// detection (slice 6) will mark a written value changed when a query hands it out. The function
// takes the components in the query's order, optionally preceded by the entity.
//
// Each World keeps a cache of the archetypes that match each query type and extends it with the
// archetypes created since the query last ran, so a query never scans archetypes it has seen.
//
// No structural change (spawn, despawn, add, remove, clear) may happen while a query iterates:
// it asserts and is refused. Gather the entities and change them afterwards; deferred commands
// arrive in slice 3.
//
// A Query refers to its World and is valid while the World lives. Not thread-safe: use it on the
// thread that owns the World.

#include "ith/ecs/Archetype.h"
#include "ith/ecs/ComponentRegistry.h"
#include "ith/ecs/Entity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ith {

class World;

namespace detail {

// The cached matches of one query type in one World.
struct QueryState {
    std::vector<ComponentIndex> terms; // in the query's order
    ComponentMask required;
    bool matchesNothing = false; // a term is not a component type
    std::size_t archetypesSeen = 0;
    std::vector<ArchetypeIndex> archetypes; // the matching archetypes
    std::vector<std::uint32_t> columns;     // per matching archetype, the column of each term
};

// Adds the archetypes created since the last update that match the query.
void updateQueryState(QueryState& state, std::span<const std::unique_ptr<Archetype>> archetypes);

// A process-wide number for each distinct list of types, which indexes the per-World caches of
// queries (the list of terms) and spawns (SpawnList and the component types).
std::size_t nextTypeListSlot() noexcept;

template <typename... Types>
std::size_t typeListSlot() noexcept {
    static const std::size_t kSlot = nextTypeListSlot();
    return kSlot;
}

// Tells the type list of a spawn apart from a query with the same types.
struct SpawnList {};

template <typename... Types>
inline constexpr bool kDistinctTypes = true;

template <typename First, typename... Rest>
inline constexpr bool kDistinctTypes<First, Rest...> = (!std::is_same_v<First, Rest> && ...) && kDistinctTypes<Rest...>;

// Counts the queries iterating a World for the duration of a scope.
class IterationScope {
public:
    explicit IterationScope(std::uint32_t& depth) noexcept : m_depth(depth) { ++m_depth; }

    ~IterationScope() { --m_depth; }

    IterationScope(const IterationScope&) = delete;
    IterationScope& operator=(const IterationScope&) = delete;
    IterationScope(IterationScope&&) = delete;
    IterationScope& operator=(IterationScope&&) = delete;

private:
    std::uint32_t& m_depth;
};

} // namespace detail

// Terms: one or more distinct component types, each `T` (write) or `const T` (read).
template <typename... Terms>
class Query {
    static_assert(sizeof...(Terms) > 0, "a query needs at least one component type");
    static_assert((!std::is_reference_v<Terms> && ...), "query terms are component types, not references");
    static_assert(detail::kDistinctTypes<std::remove_const_t<Terms>...>,
                  "a query names each component type once: it can't read and write the same component");

public:
    // Calls `function(Terms&...)` or `function(Entity, Terms&...)` for every matching entity.
    // The order is by archetype, then by row; it is deterministic for one sequence of structural
    // changes but is not otherwise specified. Structural changes inside `function` are refused.
    template <typename Function>
    void forEach(Function&& function) {
        detail::updateQueryState(*m_state, *m_archetypes);
        const detail::IterationScope scope(*m_iterationDepth);
        constexpr std::size_t kTerms = sizeof...(Terms);
        const std::uint32_t* columns = m_state->columns.data();
        for (const detail::ArchetypeIndex index : m_state->archetypes) {
            detail::Archetype& archetype = *(*m_archetypes)[index];
            if (archetype.size() != 0) {
                forEachRow(archetype, columns, function, std::index_sequence_for<Terms...>{});
            }
            columns += kTerms;
        }
    }

private:
    friend class World;

    Query(detail::QueryState& state, std::vector<std::unique_ptr<detail::Archetype>>& archetypes,
          std::uint32_t& iterationDepth) noexcept
        : m_state(&state), m_archetypes(&archetypes), m_iterationDepth(&iterationDepth) {}

    template <typename Function, std::size_t... Term>
    static void forEachRow(detail::Archetype& archetype, const std::uint32_t* columns, Function& function,
                           std::index_sequence<Term...> /*terms*/) {
        const std::tuple<Terms*...> values{static_cast<Terms*>(archetype.column(columns[Term]).data())...};
        const std::span<const Entity> entities = archetype.entities();
        const std::size_t rows = entities.size();
        for (std::size_t row = 0; row < rows; ++row) {
            if constexpr (std::is_invocable_v<Function&, Entity, Terms&...>) {
                function(entities[row], std::get<Term>(values)[row]...);
            } else {
                static_assert(std::is_invocable_v<Function&, Terms&...>,
                              "the function must take (Terms&...) or (ith::Entity, Terms&...)");
                function(std::get<Term>(values)[row]...);
            }
        }
    }

    detail::QueryState* m_state;
    std::vector<std::unique_ptr<detail::Archetype>>* m_archetypes;
    std::uint32_t* m_iterationDepth;
};

} // namespace ith
