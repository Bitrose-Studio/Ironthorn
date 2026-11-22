#include "ith/ecs/Archetype.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <utility>

namespace ith::detail {
namespace {

// Columns start at a cache line, so the first rows of different columns never share one.
constexpr std::size_t kColumnAlignment = 64;

// The first capacity of an archetype's columns.
constexpr std::size_t kInitialCapacity = 16;

} // namespace

void AlignedDelete::operator()(std::byte* memory) const noexcept {
    ::operator delete(memory, std::align_val_t(alignment));
}

void Column::reallocate(std::size_t capacity, std::size_t count) {
    ITH_ASSERT(count <= capacity);
    if (capacity > std::numeric_limits<std::size_t>::max() / m_type->size) {
        ITH_FATAL("ecs: a column would need more memory than can be addressed");
    }
    const std::size_t alignment = std::max(m_type->alignment, kColumnAlignment);
    std::unique_ptr<std::byte, AlignedDelete> memory(
        static_cast<std::byte*>(::operator new(capacity * m_type->size, std::align_val_t(alignment))),
        AlignedDelete{alignment});
    if (m_trivial) {
        if (count != 0) {
            std::memcpy(memory.get(), m_data.get(), count * m_type->size);
        }
    } else {
        for (std::size_t row = 0; row < count; ++row) {
            relocate(memory.get() + (row * m_type->size), at(row));
        }
    }
    m_data = std::move(memory);
}

void relocateValue(const TypeInfo& type, void* destination, void* source) {
    if (type.triviallyCopyable) {
        std::memcpy(destination, source, type.size);
    } else {
        type.move(destination, source);
        type.destroy(source);
    }
}

void Column::relocate(void* destination, void* source) {
    if (m_trivial) {
        std::memcpy(destination, source, m_type->size);
    } else {
        relocateValue(*m_type, destination, source);
    }
}

Archetype::Archetype(std::span<const ComponentIndex> types) : m_types(types.begin(), types.end()) {
    ITH_ASSERT_MSG(std::ranges::adjacent_find(m_types, std::greater_equal<>()) == m_types.end(),
                   "ecs: archetype types must be sorted and unique");
    m_columns.reserve(m_types.size());
    m_hooks.reserve(m_types.size());
    for (std::size_t position = 0; position < m_types.size(); ++position) {
        const ComponentIndex type = m_types[position];
        m_columns.emplace_back(componentType(type));
        m_mask.set(type);
        m_hooks.push_back(useComponentType(type));
        if (m_hooks.back() != nullptr) {
            m_hookedColumns.push_back(static_cast<std::uint32_t>(position));
        }
    }
    std::ranges::sort(m_hookedColumns, {}, [&](std::uint32_t position) { return componentType(m_types[position]).id; });
}

Archetype::~Archetype() {
    clear();
}

std::size_t Archetype::columnOf(ComponentIndex type) const noexcept {
    if (!has(type)) {
        return kNoColumn;
    }
    return static_cast<std::size_t>(std::ranges::lower_bound(m_types, type) - m_types.begin());
}

std::uint32_t Archetype::pushRow(Entity entity) {
    const std::size_t row = m_entities.size();
    if (row == std::numeric_limits<std::uint32_t>::max()) {
        ITH_FATAL("ecs: an archetype has too many rows");
    }
    if (row == m_capacity) {
        const std::size_t capacity = std::max(kInitialCapacity, m_capacity * 2);
        for (Column& column : m_columns) {
            column.reallocate(capacity, row);
        }
        m_capacity = capacity;
    }
    m_entities.push_back(entity);
    return static_cast<std::uint32_t>(row);
}

Entity Archetype::removeRow(std::uint32_t row) {
    ITH_ASSERT(row < m_entities.size());
    for (Column& column : m_columns) {
        column.destroy(column.at(row));
    }
    return fillHole(row);
}

Entity Archetype::moveRow(std::uint32_t row, Archetype& destination, std::uint32_t destinationRow) {
    ITH_ASSERT(row < m_entities.size() && destinationRow < destination.size());
    // Both type lists are sorted, so one pass pairs the columns the archetypes share.
    std::size_t target = 0;
    for (std::size_t position = 0; position < m_types.size(); ++position) {
        const ComponentIndex type = m_types[position];
        while (target < destination.m_types.size() && destination.m_types[target] < type) {
            ++target;
        }
        Column& column = m_columns[position];
        void* value = column.at(row);
        if (target < destination.m_types.size() && destination.m_types[target] == type) {
            column.relocate(destination.m_columns[target].at(destinationRow), value);
        } else {
            column.destroy(value);
        }
    }
    return fillHole(row);
}

void Archetype::clear() noexcept {
    for (Column& column : m_columns) {
        for (std::size_t row = 0; row < m_entities.size(); ++row) {
            column.destroy(column.at(row));
        }
    }
    m_entities.clear();
}

Entity Archetype::fillHole(std::uint32_t row) {
    const std::size_t last = m_entities.size() - 1;
    Entity moved;
    if (row != last) {
        for (Column& column : m_columns) {
            column.relocate(column.at(row), column.at(last));
        }
        moved = m_entities[last];
        m_entities[row] = moved;
    }
    m_entities.pop_back();
    return moved;
}

} // namespace ith::detail
