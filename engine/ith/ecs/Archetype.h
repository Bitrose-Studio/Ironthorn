#pragma once

// Archetype tables: the storage behind ith::World (ADR-0014 rule 6). Internal to the ecs module;
// no type here appears in the World's or a Query's public API, so storage can change without
// breaking callers.
//
// An archetype holds every entity with exactly one set of component types. Each component type
// has a column: one contiguous, aligned, type-erased array of values, row i belonging to the
// entity in row i. Removing a row moves the last row into the hole (swap-remove), so rows stay
// dense. Values are moved and destroyed through their reflection records.
//
// Not thread-safe: an archetype belongs to one World, which one thread uses at a time.

#include "ith/ecs/ComponentRegistry.h"
#include "ith/ecs/Entity.h"
#include "ith/reflection/TypeInfo.h"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace ith::detail {

// The position of an archetype in its World.
using ArchetypeIndex = std::uint32_t;
inline constexpr ArchetypeIndex kNoArchetype = std::numeric_limits<ArchetypeIndex>::max();

// One bit per component index.
using ComponentMask = std::bitset<kMaxComponentTypes>;

// Moves a value of `type` from `source` into uninitialized memory at `destination` and destroys
// the source; trivially copyable values are copied with memcpy.
void relocateValue(const TypeInfo& type, void* destination, void* source);

// When a component value was added and when it last changed, as World change ticks (World.h).
struct ChangeTicks {
    std::uint32_t added = 0;
    std::uint32_t changed = 0;
};

// Frees memory from aligned operator new.
struct AlignedDelete {
    std::size_t alignment = 0;
    void operator()(std::byte* memory) const noexcept;
};

// Frees change ticks from operator new.
struct TicksDelete {
    void operator()(ChangeTicks* ticks) const noexcept;
};

// One aligned, type-erased array of values of one component type. The column owns its memory
// but not the values in it: the archetype constructs and destroys them, since only it knows how
// many rows are in use.
//
// Each row also has its change ticks (ADR-0014 rule 9), in an array beside the values that
// grows, moves and swap-removes together with them.
class Column {
public:
    explicit Column(const TypeInfo& type) : m_type(&type), m_trivial(type.triviallyCopyable) {}

    // The first value; null until the column has capacity.
    [[nodiscard]] void* data() noexcept { return m_data.get(); }

    // The value in `row`. Precondition: row is below the archetype's capacity. Rows are found by
    // pointer arithmetic on the column's bytes; typed code indexes them as a T array.
    [[nodiscard]] void* at(std::size_t row) noexcept { return m_data.get() + (row * m_type->size); }

    // The change ticks of every row; null until the column has capacity.
    [[nodiscard]] ChangeTicks* ticks() noexcept { return m_ticks.get(); }

    // The change ticks of `row`. Precondition: row is below the archetype's capacity.
    [[nodiscard]] ChangeTicks& ticks(std::size_t row) noexcept { return m_ticks.get()[row]; }

    // Destroys a value of this column's type.
    void destroy(void* value) noexcept {
        if (!m_trivial) {
            m_type->destroy(value);
        }
    }

    // Moves a value of this column's type from `source` into uninitialized memory at
    // `destination` and destroys the source.
    void relocate(void* destination, void* source);

    // Moves the first `count` values and their ticks into new memory for `capacity` rows.
    // Precondition (asserted): count <= capacity.
    void reallocate(std::size_t capacity, std::size_t count);

private:
    const TypeInfo* m_type;
    bool m_trivial;
    std::unique_ptr<std::byte, AlignedDelete> m_data;
    // The change ticks of each row of capacity, uninitialized until the row is used.
    std::unique_ptr<ChangeTicks, TicksDelete> m_ticks;
};

class Archetype {
public:
    // The column position returned for a type the archetype doesn't have.
    static constexpr std::size_t kNoColumn = std::numeric_limits<std::size_t>::max();

    // An empty table for the component types `types`, which are registered, sorted and unique
    // (asserted).
    explicit Archetype(std::span<const ComponentIndex> types);

    // Destroys every value.
    ~Archetype();

    Archetype(const Archetype&) = delete;
    Archetype& operator=(const Archetype&) = delete;
    Archetype(Archetype&&) = delete;
    Archetype& operator=(Archetype&&) = delete;

    // The component types, sorted.
    [[nodiscard]] std::span<const ComponentIndex> types() const noexcept { return m_types; }

    // The component types as a bitset.
    [[nodiscard]] const ComponentMask& mask() const noexcept { return m_mask; }

    // True when the archetype has the type; constant time.
    [[nodiscard]] bool has(ComponentIndex type) const noexcept { return type < kMaxComponentTypes && m_mask[type]; }

    // The column position of `type`, or kNoColumn when the archetype doesn't have it.
    [[nodiscard]] std::size_t columnOf(ComponentIndex type) const noexcept;

    // The column at `position` (from columnOf). Precondition: position < types().size().
    [[nodiscard]] Column& column(std::size_t position) noexcept { return m_columns[position]; }

    // The hooks of the column at `position`, or null when its type has none.
    [[nodiscard]] const ComponentHooks* hooks(std::size_t position) const noexcept { return m_hooks[position]; }

    // True when a column's type has hooks.
    [[nodiscard]] bool hasHooks() const noexcept { return !m_hookedColumns.empty(); }

    // The positions of the columns with hooks, sorted by the stable type id of their types: the
    // order of despawn and clear() hooks (ADR-0030).
    [[nodiscard]] std::span<const std::uint32_t> hookedColumns() const noexcept { return m_hookedColumns; }

    // The entity in each row.
    [[nodiscard]] std::span<const Entity> entities() const noexcept { return m_entities; }

    // The number of rows.
    [[nodiscard]] std::size_t size() const noexcept { return m_entities.size(); }

    // Appends a row for `entity` and returns it. The new row's values are uninitialized: the
    // caller constructs one in every column before anything else touches the archetype. The row's
    // ticks are `tick` in every column, added and changed.
    std::uint32_t pushRow(Entity entity, std::uint32_t tick);

    // Destroys the values of `row` and fills the hole with the last row. Returns the entity
    // that moved into `row`, or the null entity when `row` was the last row.
    Entity removeRow(std::uint32_t row);

    // Moves the values of `row`, with their ticks, that `destination` has a column for into its row
    // `destinationRow` (from destination.pushRow), destroys the rest, and fills the hole as
    // removeRow does. Returns the entity that moved into `row`, or the null entity.
    Entity moveRow(std::uint32_t row, Archetype& destination, std::uint32_t destinationRow);

    // Destroys every value and removes every row; the capacity stays.
    void clear() noexcept;

    // Moves every tick older than `maxAge` ticks before `tick` up to that age, so that ticks never
    // wrap around past `tick` (World.h).
    void clampTicks(std::uint32_t tick, std::uint32_t maxAge) noexcept;

    // Cached transitions, filled by the World: the archetype with one more component type, and
    // the one with one fewer.
    std::unordered_map<ComponentIndex, ArchetypeIndex> addEdges;
    std::unordered_map<ComponentIndex, ArchetypeIndex> removeEdges;

private:
    // Moves the last row into `row`, whose values are already destroyed.
    Entity fillHole(std::uint32_t row);

    std::vector<ComponentIndex> m_types;
    std::vector<Column> m_columns;
    std::vector<const ComponentHooks*> m_hooks; // per column
    std::vector<std::uint32_t> m_hookedColumns; // see hookedColumns()
    ComponentMask m_mask;
    std::vector<Entity> m_entities;
    std::size_t m_capacity = 0;
};

} // namespace ith::detail
