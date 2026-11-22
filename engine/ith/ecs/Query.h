#pragma once

// ith::Query: typed iteration over the entities of a World (ADR-0014 rule 7).
//
//   world.query<Position, const Velocity>().forEach([](Position& position, const Velocity& velocity) {
//       position.x += velocity.x;
//   });
//   world.query<const Health>().forEach([](ith::Entity entity, const Health& health) { ... });
//   world.query<Position, ith::Without<Frozen>>().forEach([](Position& position) { ... });
//
// A query visits every entity that has all of its component types, archetype by archetype and
// row by row. A term written as `const T` reads T; a term written as plain `T` writes it. Inside a
// system (Schedule.h) every term must match the system's declared access. A writable term marks
// every value the query hands out as changed (ADR-0014 rule 9), whether the function writes it or
// not. A writable term of an immutable component type (ADR-0030) asserts, and the query visits
// nothing. The function takes the components in the query's order, optionally preceded by the
// entity.
//
// Filters narrow the entities without handing out values: ith::With<T> requires a T and
// ith::Without<T> skips entities that have one. They may stand anywhere among the terms, are not
// passed to the function, and need no declared access inside a system, because they read no
// values (like World::has). A query of filters only passes the entity, or nothing:
//
//   world.query<ith::With<Selected>>().forEach([](ith::Entity entity) { ... });
//
// Change filters keep the entities whose T was added, ith::Added<T>, or added or changed,
// ith::Changed<T>, since the running system last ran on this World (ADR-0014 rule 9). They require
// a T, like With<T>, and need read access to T inside a system. They work only while a system runs:
// outside one they assert and the query visits nothing.
//
//   context.query<const Health, ith::Changed<Health>>().forEach([](const Health& health) { ... });
//
// A query names each component type once, filters included: reading and filtering one type, or
// requiring and excluding it, is a compile error. Read a value and filter on its change with a
// term and a change filter of the same type, as above: a change filter is not a second naming.
//
// Disabled entities (Hierarchy.h, ADR-0031) are skipped: every query leaves out the entities with
// DisabledInHierarchy, unless it names Disabled or DisabledInHierarchy as a term or filter, or
// includes the filter ith::IncludeDisabled, which visits enabled and disabled entities alike (the
// editor and level saving use it).
//
//   world.query<const Position, ith::IncludeDisabled>().forEach([](const Position& position) { ... });
//
// Each World keeps a cache of the archetypes that match each query type and extends it with the
// archetypes created since the query last ran, so a query never scans archetypes it has seen.
//
// No structural change (spawn, despawn, add, remove, clear) may happen while a query iterates:
// it asserts and is refused. Record them in a command buffer (Commands.h) and apply it after the
// loop, or gather the entities and change them afterwards.
//
// Code that knows its component types only at run time uses ith::DynamicQuery (DynamicQuery.h),
// which shares the matching code.
//
// A Query refers to its World and is valid while the World lives. Not thread-safe: use it on the
// thread that owns the World.

#include "ith/ecs/Archetype.h"
#include "ith/ecs/ComponentRegistry.h"
#include "ith/ecs/Entity.h"

#include <array>
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

// A query filter: the entity must have a T; the query doesn't hand the value out. T is a plain
// component type, not const.
template <typename T>
struct With {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "With names a plain component type");
};

// A query filter: the entity must not have a T. T is a plain component type, not const.
template <typename T>
struct Without {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Without names a plain component type");
};

// A query filter: the query visits disabled entities too (see the header comment).
struct IncludeDisabled {};

// A change filter: the entity's T was added since the running system last ran. T is a plain
// component type, not const.
template <typename T>
struct Added {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Added names a plain component type");
};

// A change filter: the entity's T was added or changed since the running system last ran. T is a
// plain component type, not const.
template <typename T>
struct Changed {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "Changed names a plain component type");
};

template <typename... Terms>
class Query;

namespace detail {

// A World's change clock (World.h, ADR-0014 rule 9). Writes are stamped with `tick`. While a
// system runs, `tick` is the system's current run and `lastRun` its previous run on this World;
// Added and Changed keep the ticks after lastRun, up to tick.
struct ChangeClock {
    std::uint32_t tick = 0;
    std::uint32_t lastRun = 0;
    bool inSystem = false;
};

// How often, in ticks, a World clamps its old ticks, and the age they are clamped to. A tick is at
// most kMaxChangeAge + kTickCheckInterval old, so it never wraps around past the current tick.
inline constexpr std::uint32_t kTickCheckInterval = 1u << 29;
inline constexpr std::uint32_t kMaxChangeAge = 0xFFFFFFFFu - (2u * kTickCheckInterval);

// True when `tick` is after `lastRun` and not after `thisRun`. Ages are counted back from thisRun,
// so the comparison holds across the 32-bit wraparound. A tick older than kMaxChangeAge counts as
// that old; a lastRun of kMaxChangeAge + 1 ticks ago (a system's first run) keeps every tick.
[[nodiscard]] inline bool isNewer(std::uint32_t tick, std::uint32_t lastRun, std::uint32_t thisRun) noexcept {
    const std::uint32_t age = thisRun - tick;
    return (age < kMaxChangeAge ? age : kMaxChangeAge) < thisRun - lastRun;
}

// True when a query may use change filters: a system runs. Asserts otherwise.
[[nodiscard]] bool changeFiltersAllowed(const ChangeClock& clock) noexcept;

// The cached matches of one query type in one World.
struct QueryState {
    std::vector<ComponentIndex> terms;   // the terms that hand out values, in the query's order
    std::vector<ComponentIndex> changes; // the types of the change filters, in the query's order
    std::vector<std::uint8_t> added;     // per change filter: 1 for Added, 0 for Changed
    ComponentMask required;              // the terms, the With filters and the change filters
    ComponentMask excluded;              // the Without filters, and DisabledInHierarchy by default
    bool matchesNothing = false;         // a term or filter is not a component type, or is repeated
    std::size_t archetypesSeen = 0;
    std::vector<ArchetypeIndex> archetypes; // the matching archetypes
    // Per matching archetype, the column of each term, then of each change filter.
    std::vector<std::uint32_t> columns;
};

// Fills a new query state with its terms and filters. A type that is kNoComponent (it is not a
// component; registering it asserted) or that repeats an earlier term or filter (asserted) makes
// the query match nothing; a change filter may name the type of a term or With filter. Unless
// `includeDisabled` is set or the query names Disabled or DisabledInHierarchy, DisabledInHierarchy
// is excluded. No terms and no filters match every enabled archetype.
void initQueryState(QueryState& state, std::span<const ComponentIndex> terms, std::span<const ComponentIndex> with = {},
                    std::span<const ComponentIndex> without = {}, bool includeDisabled = false,
                    std::span<const ComponentIndex> added = {}, std::span<const ComponentIndex> changed = {});

// True when the entity in `row` passes every change filter of `state`, whose columns in this
// archetype start at `filterColumns`.
[[nodiscard]] inline bool passesChangeFilters(Archetype& archetype, const QueryState& state,
                                              const std::uint32_t* filterColumns, std::size_t row,
                                              const ChangeClock& clock) noexcept {
    for (std::size_t filter = 0; filter < state.changes.size(); ++filter) {
        const ChangeTicks& ticks = archetype.column(filterColumns[filter]).ticks(row);
        if (!isNewer(state.added[filter] != 0 ? ticks.added : ticks.changed, clock.lastRun, clock.tick)) {
            return false;
        }
    }
    return true;
}

// Adds the archetypes created since the last update that have every required type and no
// excluded one.
void updateQueryState(QueryState& state, std::span<const std::unique_ptr<Archetype>> archetypes);

// A process-wide number for each distinct list of types, which indexes the per-World caches of
// queries (the list of terms and filters) and spawns (SpawnList and the component types).
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

// Which query terms are filters.
template <typename T>
inline constexpr bool kIsWith = false;
template <typename T>
inline constexpr bool kIsWith<With<T>> = true;

template <typename T>
inline constexpr bool kIsWithout = false;
template <typename T>
inline constexpr bool kIsWithout<Without<T>> = true;

template <typename T>
inline constexpr bool kIsIncludeDisabled = std::is_same_v<T, IncludeDisabled>;

template <typename T>
inline constexpr bool kIsAdded = false;
template <typename T>
inline constexpr bool kIsAdded<Added<T>> = true;

template <typename T>
inline constexpr bool kIsChanged = false;
template <typename T>
inline constexpr bool kIsChanged<Changed<T>> = true;

template <typename T>
inline constexpr bool kIsChangeFilter = kIsAdded<T> || kIsChanged<T>;

template <typename T>
inline constexpr bool kIsFilter = kIsWith<T> || kIsWithout<T> || kIsIncludeDisabled<T> || kIsChangeFilter<T>;

// The component type that a change filter names.
template <typename T>
struct ChangeFilterComponent;

template <typename T>
struct ChangeFilterComponent<Added<T>> {
    using Type = T;
};

template <typename T>
struct ChangeFilterComponent<Changed<T>> {
    using Type = T;
};

// The component type that a term or a filter names, without const; for the purpose of naming
// each type once. A change filter stays itself, so it may repeat the type of a term.
template <typename T>
struct TermComponent {
    using Type = std::remove_const_t<T>;
};

template <typename T>
struct TermComponent<With<T>> {
    using Type = T;
};

template <typename T>
struct TermComponent<Without<T>> {
    using Type = T;
};

template <typename T>
using TermComponentT = TermComponent<T>::Type;

// The Query type of a list of terms and filters: Query<the terms that hand out values, in order>.
template <typename Done, typename... Rest>
struct QueryOfTerms {
    using Type = Done;
};

template <typename... Done, typename First, typename... Rest>
struct QueryOfTerms<Query<Done...>, First, Rest...>
    : QueryOfTerms<std::conditional_t<kIsFilter<First>, Query<Done...>, Query<Done..., First>>, Rest...> {};

template <typename... Terms>
using QueryOf = QueryOfTerms<Query<>, Terms...>::Type;

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

// Terms: distinct component types, each `T` (write) or `const T` (read); none for a query of
// filters only. World::query builds it from its terms and filters, which it keeps out of Terms.
template <typename... Terms>
class Query {
    static_assert((!std::is_reference_v<Terms> && ...), "query terms are component types, not references");
    static_assert(detail::kDistinctTypes<std::remove_const_t<Terms>...>,
                  "a query names each component type once: it can't read and write the same component");

public:
    // Calls `function(Terms&...)` or `function(Entity, Terms&...)` for every matching entity;
    // filters are not passed. The order is by archetype, then by row; it is deterministic for one
    // sequence of structural changes but is not otherwise specified. Structural changes inside
    // `function` are refused.
    template <typename Function>
    void forEach(Function&& function) {
        detail::updateQueryState(*m_state, *m_archetypes);
        const std::size_t filters = m_state->changes.size();
        if (filters != 0 && !detail::changeFiltersAllowed(*m_clock)) {
            return;
        }
        const detail::IterationScope scope(*m_iterationDepth);
        const std::size_t stride = sizeof...(Terms) + filters;
        const std::uint32_t* columns = m_state->columns.data();
        for (const detail::ArchetypeIndex index : m_state->archetypes) {
            detail::Archetype& archetype = *(*m_archetypes)[index];
            if (archetype.size() != 0) {
                forEachRow(archetype, columns, filters != 0, function, std::index_sequence_for<Terms...>{});
            }
            columns += stride;
        }
    }

private:
    friend class World;

    Query(detail::QueryState& state, std::vector<std::unique_ptr<detail::Archetype>>& archetypes,
          std::uint32_t& iterationDepth, const detail::ChangeClock& clock) noexcept
        : m_state(&state), m_archetypes(&archetypes), m_iterationDepth(&iterationDepth), m_clock(&clock) {}

    template <typename Function, std::size_t... Term>
    void forEachRow(detail::Archetype& archetype, const std::uint32_t* columns, bool filtered, Function& function,
                    std::index_sequence<Term...> /*terms*/) const {
        [[maybe_unused]] const std::tuple<Terms*...> values{
            static_cast<Terms*>(archetype.column(columns[Term]).data())...};
        // Written terms mark each value they hand out as changed.
        [[maybe_unused]] const std::array<detail::ChangeTicks*, sizeof...(Terms)> ticks{
            (std::is_const_v<Terms> ? nullptr : archetype.column(columns[Term]).ticks())...};
        [[maybe_unused]] const std::uint32_t tick = m_clock->tick;
        const std::span<const Entity> entities = archetype.entities();
        const std::size_t rows = entities.size();
        for (std::size_t row = 0; row < rows; ++row) {
            if (filtered &&
                !detail::passesChangeFilters(archetype, *m_state, columns + sizeof...(Terms), row, *m_clock)) {
                continue;
            }
            [[maybe_unused]] const auto mark = [&]<std::size_t Index, typename Value> {
                if constexpr (!std::is_const_v<Value>) {
                    ticks[Index][row].changed = tick;
                }
            };
            (mark.template operator()<Term, Terms>(), ...);
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
    const detail::ChangeClock* m_clock;
};

} // namespace ith
